/*-------------------------------------------------------------------------
 * fe-auth-passkey.c
 *	  Client-side Passkey SASL authentication
 *
 * This implements the SASL state machine for passkey authentication.
 * Platform-specific code (macOS AuthenticationServices, Windows Hello)
 * is in separate files.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-passkey.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/passkey.h"
#include "libpq/cable.h"

#include <string.h>

#include "fe-auth-passkey.h"

#define PGPASSKEYDEBUG_ENV "PGPASSKEYDEBUG"

#define passkey_debug(...) do { \
	if (getenv(PGPASSKEYDEBUG_ENV)) fprintf(stderr, __VA_ARGS__); \
} while(0)

/* Client state machine states */
typedef enum {
	PASSKEY_CLIENT_STATE_INIT = 0,
	PASSKEY_CLIENT_STATE_PASSWORD_REQUESTED,
	PASSKEY_CLIENT_STATE_CHALLENGE_RECEIVED,
	PASSKEY_CLIENT_STATE_FINISHED
} passkey_client_state;

typedef struct {
	PGconn	   *conn;
	passkey_client_state state;
	char	   *rp_id;
	uint8_t		challenge[PASSKEY_CHALLENGE_LENGTH];
	uint8_t		options;
	uint8_t	   *credential_id;
	size_t		credential_id_len;
	bool		allow_hybrid;
	uint8_t		operation;			/* PASSKEY_OP_GET_ASSERTION or PASSKEY_OP_MAKE_CREDENTIAL */
	uint8_t	   *user_id;
	size_t		user_id_len;
	char	   *user_name;
	char	   *password;			/* password from .pgpass or connection string */
} fe_passkey_state;

static void *passkey_init(PGconn *conn, const char *password, const char *mech);
static SASLStatus passkey_exchange(void *state, bool final,
								   char *input, int inputlen,
								   char **output, int *outputlen);
static bool passkey_channel_bound(void *state);
static void passkey_free(void *state);

const pg_fe_sasl_mech pg_passkey_mech = {
	passkey_init, passkey_exchange, passkey_channel_bound, passkey_free
};

static void *
passkey_init(PGconn *conn, const char *password, const char *mech)
{
	fe_passkey_state *st;

	/* Check if passkey is supported on this platform */
	if (!pg_passkey_supported())
	{
		libpq_append_conn_error(conn, "passkey authentication is not supported on this platform");
		return NULL;
	}

	passkey_debug("PASSKEY: init called, password=%s, pgpass=%s\n",
				  password ? "(set)" : "(null)",
				  conn->pgpass ? "(set)" : "(null)");

	st = malloc(sizeof(fe_passkey_state));
	if (!st)
		return NULL;
	memset(st, 0, sizeof(fe_passkey_state));
	st->conn = conn;

	/* Store password for later use in SASL exchange */
	if (password && password[0] != '\0')
		st->password = strdup(password);
	else if (conn->pgpass && conn->pgpass[0] != '\0')
		st->password = strdup(conn->pgpass);

	/* Default to allowing hybrid transport (QR code) */
	st->allow_hybrid = true;

	/* Check for connection parameter to disable hybrid */
	if (conn->passkey_hybrid && strcmp(conn->passkey_hybrid, "0") == 0)
		st->allow_hybrid = false;

	return st;
}

static SASLStatus
passkey_exchange(void *opaque, bool final, char *input, int inputlen,
				 char **output, int *outputlen)
{
	fe_passkey_state *st = (fe_passkey_state *) opaque;

	*output = NULL;
	*outputlen = 0;

	if (st->state == PASSKEY_CLIENT_STATE_INIT)
	{
		/*
		 * Client-first message: send empty to initiate SASL exchange.
		 */
		passkey_debug("PASSKEY: sending client-first-message (empty)\n");

		*output = malloc(1);
		if (!*output)
			return SASL_FAILED;
		*outputlen = 0;
		st->state = PASSKEY_CLIENT_STATE_PASSWORD_REQUESTED;
		return SASL_CONTINUE;
	}

	if (st->state == PASSKEY_CLIENT_STATE_PASSWORD_REQUESTED)
	{
		/*
		 * Server requests password.
		 * Parse: msg_type(1) + version(1)
		 * Send: msg_type(1) + password_len(2) + password
		 */
		const uint8_t *p = (const uint8_t *) input;
		const uint8_t *end = p + inputlen;
		const char *password;
		size_t		pass_len;
		uint8_t	   *resp;
		size_t		resp_len;

		if (p + 2 > end || p[0] != PASSKEY_MSG_PASSWORD_REQUEST)
		{
			libpq_append_conn_error(st->conn, "expected password request from server");
			return SASL_FAILED;
		}

		if (p[1] != PASSKEY_PROTOCOL_VERSION)
		{
			libpq_append_conn_error(st->conn, "invalid passkey protocol version");
			return SASL_FAILED;
		}

		passkey_debug("PASSKEY: server requested password\n");

		/*
		 * Use password stored during init (from .pgpass or connection string).
		 */
		password = st->password;
		if (!password || password[0] == '\0')
		{
			libpq_append_conn_error(st->conn, "password required for passkey authentication");
			return SASL_FAILED;
		}

		pass_len = strlen(password);

		/*
		 * Build password response:
		 * msg_type(1) + password_len(2) + password
		 */
		resp_len = 1 + 2 + pass_len;
		resp = malloc(resp_len);
		if (!resp)
			return SASL_FAILED;

		resp[0] = PASSKEY_MSG_PASSWORD_RESPONSE;
		resp[1] = (pass_len >> 8) & 0xFF;
		resp[2] = pass_len & 0xFF;
		memcpy(resp + 3, password, pass_len);

		passkey_debug("PASSKEY: sending password (%zu bytes)\n", pass_len);

		*output = (char *) resp;
		*outputlen = resp_len;
		st->state = PASSKEY_CLIENT_STATE_CHALLENGE_RECEIVED;
		return SASL_CONTINUE;
	}

	if (st->state == PASSKEY_CLIENT_STATE_CHALLENGE_RECEIVED)
	{
		/*
		 * Parse server passkey challenge:
		 * msg_type(1) + version(1) + operation(1) + challenge(32) +
		 * rp_id_len(2) + rp_id + options(1) + cred_id_len(2) + cred_id +
		 * [for registration: user_id_len(2) + user_id + user_name_len(2) + user_name]
		 */
		const uint8_t *p = (const uint8_t *) input;
		const uint8_t *end = p + inputlen;
		uint16_t	rp_id_len, cred_id_len;
		uint8_t	   *resp;
		size_t		resp_len;

		/* Check message type */
		if (p + 1 > end || p[0] != PASSKEY_MSG_PASSKEY_CHALLENGE)
		{
			libpq_append_conn_error(st->conn, "expected passkey challenge from server");
			return SASL_FAILED;
		}
		p++;

		/* Check version */
		if (p + 1 > end || p[0] != PASSKEY_PROTOCOL_VERSION)
		{
			libpq_append_conn_error(st->conn, "invalid passkey protocol version");
			return SASL_FAILED;
		}
		p++;

		/* Parse operation type */
		if (p + 1 > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		st->operation = *p++;

		/* Parse challenge */
		if (p + PASSKEY_CHALLENGE_LENGTH > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		memcpy(st->challenge, p, PASSKEY_CHALLENGE_LENGTH);
		p += PASSKEY_CHALLENGE_LENGTH;

		/* Parse rp_id */
		if (p + 2 > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		rp_id_len = ((uint16_t) p[0] << 8) | p[1];
		p += 2;
		if (p + rp_id_len > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		st->rp_id = malloc(rp_id_len + 1);
		if (!st->rp_id)
			return SASL_FAILED;
		memcpy(st->rp_id, p, rp_id_len);
		st->rp_id[rp_id_len] = '\0';
		p += rp_id_len;

		/* Parse options */
		if (p + 1 > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		st->options = *p++;

		/* Parse credential_id */
		if (p + 2 > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		cred_id_len = ((uint16_t) p[0] << 8) | p[1];
		p += 2;
		if (p + cred_id_len > end)
		{
			libpq_append_conn_error(st->conn, "invalid challenge message");
			return SASL_FAILED;
		}
		if (cred_id_len > 0)
		{
			st->credential_id = malloc(cred_id_len);
			if (!st->credential_id)
				return SASL_FAILED;
			memcpy(st->credential_id, p, cred_id_len);
			st->credential_id_len = cred_id_len;
		}
		p += cred_id_len;

		/* For registration, parse user info */
		if (st->operation == PASSKEY_OP_MAKE_CREDENTIAL)
		{
			uint16_t	user_id_len, user_name_len;

			if (p + 2 > end)
			{
				libpq_append_conn_error(st->conn, "invalid challenge message");
				return SASL_FAILED;
			}
			user_id_len = ((uint16_t) p[0] << 8) | p[1];
			p += 2;
			if (p + user_id_len > end)
			{
				libpq_append_conn_error(st->conn, "invalid challenge message");
				return SASL_FAILED;
			}
			st->user_id = malloc(user_id_len);
			if (!st->user_id)
				return SASL_FAILED;
			memcpy(st->user_id, p, user_id_len);
			st->user_id_len = user_id_len;
			p += user_id_len;

			if (p + 2 > end)
			{
				libpq_append_conn_error(st->conn, "invalid challenge message");
				return SASL_FAILED;
			}
			user_name_len = ((uint16_t) p[0] << 8) | p[1];
			p += 2;
			if (p + user_name_len > end)
			{
				libpq_append_conn_error(st->conn, "invalid challenge message");
				return SASL_FAILED;
			}
			st->user_name = malloc(user_name_len + 1);
			if (!st->user_name)
				return SASL_FAILED;
			memcpy(st->user_name, p, user_name_len);
			st->user_name[user_name_len] = '\0';

			passkey_debug("PASSKEY: received MakeCredential challenge (rp_id=%s, user=%s)\n",
						  st->rp_id, st->user_name);
		}
		else
		{
			passkey_debug("PASSKEY: received GetAssertion challenge (rp_id=%s, options=0x%02x, cred_id_len=%zu)\n",
						  st->rp_id, st->options, st->credential_id_len);
		}

		/*
		 * Perform passkey operation based on operation type.
		 */
		if (st->operation == PASSKEY_OP_MAKE_CREDENTIAL)
		{
			PasskeyAttestation *attestation;

			/*
			 * Perform MakeCredential (registration).
			 */
			attestation = cable_make_credential(st->rp_id,
												st->rp_id,	/* Use rp_id as rp_name */
												st->user_id,
												st->user_id_len,
												st->user_name,
												st->user_name,	/* Use user_name as display_name */
												st->challenge,
												PASSKEY_CHALLENGE_LENGTH);
			if (!attestation)
			{
				libpq_append_conn_error(st->conn, "failed to create passkey credential");
				return SASL_FAILED;
			}

			if (attestation->error_message)
			{
				libpq_append_conn_error(st->conn, "passkey error: %s", attestation->error_message);
				cable_free_attestation(attestation);
				return SASL_FAILED;
			}

			/*
			 * Build response:
			 * msg_type(1) + authenticator_data_len(2) + authenticator_data +
			 * client_data_json_len(2) + client_data_json +
			 * credential_id_len(2) + credential_id +
			 * public_key_len(2) + public_key
			 */
			resp_len = 1 + 2 + attestation->authenticator_data_len +
					   2 + attestation->client_data_json_len +
					   2 + attestation->credential_id_len +
					   2 + attestation->public_key_len;

			resp = malloc(resp_len);
			if (!resp)
			{
				cable_free_attestation(attestation);
				return SASL_FAILED;
			}

			{
				uint8_t	   *w = resp;

				/* msg_type */
				*w++ = PASSKEY_MSG_PASSKEY_RESPONSE;

				/* authenticator_data */
				*w++ = (attestation->authenticator_data_len >> 8) & 0xFF;
				*w++ = attestation->authenticator_data_len & 0xFF;
				memcpy(w, attestation->authenticator_data, attestation->authenticator_data_len);
				w += attestation->authenticator_data_len;

				/* client_data_json */
				*w++ = (attestation->client_data_json_len >> 8) & 0xFF;
				*w++ = attestation->client_data_json_len & 0xFF;
				memcpy(w, attestation->client_data_json, attestation->client_data_json_len);
				w += attestation->client_data_json_len;

				/* credential_id */
				*w++ = (attestation->credential_id_len >> 8) & 0xFF;
				*w++ = attestation->credential_id_len & 0xFF;
				memcpy(w, attestation->credential_id, attestation->credential_id_len);
				w += attestation->credential_id_len;

				/* public_key */
				*w++ = (attestation->public_key_len >> 8) & 0xFF;
				*w++ = attestation->public_key_len & 0xFF;
				memcpy(w, attestation->public_key, attestation->public_key_len);
			}

			passkey_debug("PASSKEY: sending attestation (auth_data=%zu, cred_id=%zu, pubkey=%zu)\n",
						  attestation->authenticator_data_len,
						  attestation->credential_id_len,
						  attestation->public_key_len);

			cable_free_attestation(attestation);
		}
		else
		{
			PasskeyAssertion *assertion;

			/*
			 * Perform GetAssertion (authentication).
			 */
			assertion = pg_passkey_get_assertion(st->rp_id,
												 st->challenge,
												 PASSKEY_CHALLENGE_LENGTH,
												 st->credential_id,
												 st->credential_id_len,
												 st->allow_hybrid);
			if (!assertion)
			{
				libpq_append_conn_error(st->conn, "failed to get passkey assertion");
				return SASL_FAILED;
			}

			if (assertion->error_message)
			{
				libpq_append_conn_error(st->conn, "passkey error: %s", assertion->error_message);
				pg_passkey_free_assertion(assertion);
				return SASL_FAILED;
			}

			/*
			 * Build response:
			 * msg_type(1) + authenticator_data_len(2) + authenticator_data +
			 * client_data_json_len(2) + client_data_json +
			 * signature_len(2) + signature +
			 * credential_id_len(2) + credential_id
			 */
			resp_len = 1 + 2 + assertion->authenticator_data_len +
					   2 + assertion->client_data_json_len +
					   2 + assertion->signature_len +
					   2 + assertion->credential_id_len;

			resp = malloc(resp_len);
			if (!resp)
			{
				pg_passkey_free_assertion(assertion);
				return SASL_FAILED;
			}

			{
				uint8_t	   *w = resp;

				/* msg_type */
				*w++ = PASSKEY_MSG_PASSKEY_RESPONSE;

				/* authenticator_data */
				*w++ = (assertion->authenticator_data_len >> 8) & 0xFF;
				*w++ = assertion->authenticator_data_len & 0xFF;
				memcpy(w, assertion->authenticator_data, assertion->authenticator_data_len);
				w += assertion->authenticator_data_len;

				/* client_data_json */
				*w++ = (assertion->client_data_json_len >> 8) & 0xFF;
				*w++ = assertion->client_data_json_len & 0xFF;
				memcpy(w, assertion->client_data_json, assertion->client_data_json_len);
				w += assertion->client_data_json_len;

				/* signature */
				*w++ = (assertion->signature_len >> 8) & 0xFF;
				*w++ = assertion->signature_len & 0xFF;
				memcpy(w, assertion->signature, assertion->signature_len);
				w += assertion->signature_len;

				/* credential_id */
				*w++ = (assertion->credential_id_len >> 8) & 0xFF;
				*w++ = assertion->credential_id_len & 0xFF;
				memcpy(w, assertion->credential_id, assertion->credential_id_len);
			}

			passkey_debug("PASSKEY: sending assertion (auth_data=%zu, client_data=%zu, sig=%zu, cred_id=%zu)\n",
						  assertion->authenticator_data_len, assertion->client_data_json_len,
						  assertion->signature_len, assertion->credential_id_len);

			pg_passkey_free_assertion(assertion);
		}

		*output = (char *) resp;
		*outputlen = resp_len;
		st->state = PASSKEY_CLIENT_STATE_FINISHED;
		return final ? SASL_COMPLETE : SASL_CONTINUE;
	}

	return final ? SASL_COMPLETE : SASL_FAILED;
}

static bool
passkey_channel_bound(void *state)
{
	return false;
}

static void
passkey_free(void *opaque)
{
	fe_passkey_state *st = (fe_passkey_state *) opaque;

	if (!st)
		return;

	free(st->rp_id);
	free(st->credential_id);
	free(st->user_id);
	free(st->user_name);
	if (st->password)
	{
		explicit_bzero(st->password, strlen(st->password));
		free(st->password);
	}
	free(st);
}

/*
 * caBLE-based passkey implementation.
 *
 * When OpenSSL is available, we use caBLE (Cloud-Assisted BLE) to enable
 * cross-device passkey authentication via QR code. This works on all
 * platforms without requiring platform-specific entitlements.
 *
 * On macOS, we also support the native AuthenticationServices framework
 * as a fallback (implemented in fe-auth-passkey-darwin.m), but caBLE is
 * preferred for CLI tools because AuthenticationServices requires app
 * bundle entitlements that psql cannot have.
 */

#ifdef USE_OPENSSL

bool
pg_passkey_supported(void)
{
	/* caBLE is always supported when OpenSSL is available */
	return true;
}

PasskeyAssertion *
pg_passkey_get_assertion(const char *rp_id,
						 const uint8_t *challenge,
						 size_t challenge_len,
						 const uint8_t *credential_id,
						 size_t credential_id_len,
						 bool allow_hybrid)
{
	passkey_debug("PASSKEY: using caBLE transport for cross-device authentication\n");
	passkey_debug("PASSKEY: credential_id_len=%zu\n", credential_id_len);

	/*
	 * Use caBLE for passkey authentication.
	 * This will display a QR code for the user to scan with their phone.
	 * The credential_id is passed to include in the allowCredentials list,
	 * which is required for non-discoverable credentials.
	 */
	return cable_get_assertion(rp_id, challenge, challenge_len,
							   credential_id, credential_id_len);
}

void
pg_passkey_free_assertion(PasskeyAssertion *assertion)
{
	if (!assertion)
		return;
	free(assertion->authenticator_data);
	free(assertion->client_data_json);
	free(assertion->signature);
	free(assertion->credential_id);
	free(assertion->error_message);
	free(assertion);
}

#elif !defined(__APPLE__)

/*
 * Stub implementation for platforms without OpenSSL or Darwin support.
 */

bool
pg_passkey_supported(void)
{
	return false;
}

PasskeyAssertion *
pg_passkey_get_assertion(const char *rp_id,
						 const uint8_t *challenge,
						 size_t challenge_len,
						 const uint8_t *credential_id,
						 size_t credential_id_len,
						 bool allow_hybrid)
{
	PasskeyAssertion *assertion = malloc(sizeof(PasskeyAssertion));
	if (assertion)
	{
		memset(assertion, 0, sizeof(PasskeyAssertion));
		assertion->error_message = strdup("passkey requires OpenSSL support");
	}
	return assertion;
}

void
pg_passkey_free_assertion(PasskeyAssertion *assertion)
{
	if (!assertion)
		return;
	free(assertion->authenticator_data);
	free(assertion->client_data_json);
	free(assertion->signature);
	free(assertion->credential_id);
	free(assertion->error_message);
	free(assertion);
}

#endif							/* USE_OPENSSL / !__APPLE__ */
