/*-------------------------------------------------------------------------
 * fe-auth-fido2.c
 *	  Client-side FIDO2 SASL authentication using OpenSSH sk-api
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-fido2.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/fido2.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

#include <string.h>

#include "fe-auth-fido2.h"
#include "port/pg_bswap.h"

#define PGFIDO2DEBUG_ENV "PGFIDO2DEBUG"

#define fido2_debug(...) do { \
	if (getenv(PGFIDO2DEBUG_ENV)) fprintf(stderr, __VA_ARGS__); \
} while(0)

/* Client state machine states */
typedef enum {
	FIDO2_CLIENT_STATE_INIT = 0,
	FIDO2_CLIENT_STATE_CHALLENGE_RECEIVED,
	FIDO2_CLIENT_STATE_FINISHED
} fido2_client_state;

#ifndef WIN32

typedef struct {
	uint8_t	   *public_key;
	size_t		public_key_len;
	uint8_t	   *key_handle;
	size_t		key_handle_len;
} fe_fido2_key;

typedef struct {
	PGconn	   *conn;
	void	   *handle;
	sk_sign_fn	sign;
	sk_load_resident_keys_fn load_keys;
	sk_free_sign_response_fn free_sign_response;
	sk_free_resident_keys_fn free_resident_keys;
	fe_fido2_key *keys;
	size_t		num_keys;
	size_t		key_idx;
	uint8_t		challenge[FIDO2_CHALLENGE_LENGTH];
	uint8_t		options;
	fido2_client_state state;
} fe_fido2_state;

static void *fido2_init(PGconn *conn, const char *password, const char *mech);
static SASLStatus fido2_exchange(void *state, bool final,
								  char *input, int inputlen,
								  char **output, int *outputlen);
static bool fido2_channel_bound(void *state);
static void fido2_free(void *state);

const pg_fe_sasl_mech pg_fido2_mech = {
	fido2_init, fido2_exchange, fido2_channel_bound, fido2_free
};

static void *
fido2_init(PGconn *conn, const char *password, const char *mech)
{
	fe_fido2_state *st;
	const char *path;
	sk_api_version_fn version_fn;
	struct sk_resident_key **rks = NULL;
	size_t		nrks = 0, i, n;
	const char *key_str;

	path = conn->sk_provider;
	if (!path || !path[0])
		path = getenv("PGSKPROVIDER");
	if (!path || !path[0])
	{
		libpq_append_conn_error(conn, "sk_provider or PGSKPROVIDER required");
		return NULL;
	}

	st = malloc(sizeof(fe_fido2_state));
	if (!st)
		return NULL;
	memset(st, 0, sizeof(fe_fido2_state));
	st->conn = conn;

	st->handle = dlopen(path, RTLD_NOW);
	if (!st->handle)
	{
		libpq_append_conn_error(conn, "failed to load fido2-provider: %s", dlerror());
		free(st);
		return NULL;
	}

	version_fn = (sk_api_version_fn) dlsym(st->handle, "sk_api_version");
	st->sign = (sk_sign_fn) dlsym(st->handle, "sk_sign");
	st->load_keys = (sk_load_resident_keys_fn) dlsym(st->handle, "sk_load_resident_keys");
	st->free_sign_response = (sk_free_sign_response_fn) dlsym(st->handle, "sk_free_sign_response");
	st->free_resident_keys = (sk_free_resident_keys_fn) dlsym(st->handle, "sk_free_resident_keys");

	if (!version_fn || !st->sign || !st->load_keys ||
		(version_fn() & SSH_SK_VERSION_MAJOR_MASK) != SSH_SK_VERSION_MAJOR)
	{
		libpq_append_conn_error(conn, "invalid fido2-provider");
		dlclose(st->handle);
		free(st);
		return NULL;
	}

	if (st->load_keys(conn->fido2_pin, NULL, &rks, &nrks) != 0 || !rks)
	{
		libpq_append_conn_error(conn, "failed to load resident keys");
		dlclose(st->handle);
		free(st);
		return NULL;
	}

	/* Count and copy matching keys in single pass */
	for (i = 0; i < nrks; i++)
		if (rks[i]->application && strcmp(rks[i]->application, FIDO2_RP_ID) == 0)
			st->num_keys++;

	if (st->num_keys == 0)
	{
		libpq_append_conn_error(conn, "no ssh: credentials on security key");
		goto free_rks;
	}

	st->keys = malloc(sizeof(fe_fido2_key) * st->num_keys);
	if (!st->keys)
		goto free_rks;

	for (i = 0, n = 0; i < nrks; i++)
	{
		if (rks[i]->application && strcmp(rks[i]->application, FIDO2_RP_ID) == 0)
		{
			fe_fido2_key *k = &st->keys[n];
			k->public_key_len = rks[i]->key.public_key_len;
			k->public_key = malloc(k->public_key_len);
			k->key_handle_len = rks[i]->key.key_handle_len;
			k->key_handle = malloc(k->key_handle_len);
			if (!k->public_key || !k->key_handle)
			{
				/* Clean up partial allocation */
				free(k->public_key);
				free(k->key_handle);
				k->public_key = NULL;
				k->key_handle = NULL;
				/* Continue to free rks[i] below, but don't increment n */
			}
			else
			{
				memcpy(k->public_key, rks[i]->key.public_key, k->public_key_len);
				memcpy(k->key_handle, rks[i]->key.key_handle, k->key_handle_len);
				n++;
			}
		}
	}
	if (st->free_resident_keys)
		st->free_resident_keys(rks, nrks);
	else
	{
		for (i = 0; i < nrks; i++)
		{
			free(rks[i]->application);
			free(rks[i]->key.public_key);
			free(rks[i]->key.key_handle);
			free(rks[i]->user_id);
			free(rks[i]);
		}
		free(rks);
	}

	/* Update num_keys to reflect actually copied keys */
	st->num_keys = n;
	if (st->num_keys == 0)
	{
		libpq_append_conn_error(conn, "failed to load credentials from security key");
		free(st->keys);
		st->keys = NULL;
		dlclose(st->handle);
		free(st);
		return NULL;
	}

	/* Handle fido2_credential selection */
	key_str = conn->fido2_credential;
	if (key_str && key_str[0])
	{
		Fido2ParsedPubkey parsed;
		char *err = NULL;
		bool found = false;

		if (!fido2_parse_openssh_pubkey(key_str, &parsed, &err))
		{
			libpq_append_conn_error(conn, "invalid fido2_credential: %s",
									err ? err : "parse error");
			free(err);
			st->state = FIDO2_CLIENT_STATE_FINISHED;	/* suppress HINT in fido2_free */
			fido2_free(st);
			return NULL;
		}
		free(err);

		for (i = 0; i < st->num_keys; i++)
		{
			if (st->keys[i].public_key_len == parsed.public_key_len &&
				memcmp(st->keys[i].public_key, parsed.public_key, parsed.public_key_len) == 0)
			{
				st->key_idx = i;
				found = true;
				break;
			}
		}
		fido2_free_parsed_pubkey(&parsed);

		if (!found)
		{
			libpq_append_conn_error(conn, "fido2_credential not found on security key");
			st->state = FIDO2_CLIENT_STATE_FINISHED;	/* suppress HINT in fido2_free */
			fido2_free(st);
			return NULL;
		}
	}

	return st;

free_rks:
	if (st->free_resident_keys)
		st->free_resident_keys(rks, nrks);
	else
	{
		for (i = 0; i < nrks; i++)
		{
			free(rks[i]->application);
			free(rks[i]->key.public_key);
			free(rks[i]->key.key_handle);
			free(rks[i]->user_id);
			free(rks[i]);
		}
		free(rks);
	}
	dlclose(st->handle);
	free(st);
	return NULL;
}

static SASLStatus
fido2_exchange(void *opaque, bool final, char *input, int inputlen,
				char **output, int *outputlen)
{
	fe_fido2_state *st = (fe_fido2_state *) opaque;
	fe_fido2_key *key;
	const uint8_t *p;
	struct sk_sign_response *sig = NULL;
	uint8_t		ext[FIDO2_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH];
	uint8_t		rp_hash[PG_SHA256_DIGEST_LENGTH];
	pg_cryptohash_ctx *ctx;
	uint8_t		flags;
	uint8_t	   *out;

	*output = NULL;
	*outputlen = 0;

	if (st->state == FIDO2_CLIENT_STATE_INIT)
	{
		/* Send public key */
		key = &st->keys[st->key_idx];
		*output = malloc(key->public_key_len);
		if (!*output)
			return SASL_FAILED;
		memcpy(*output, key->public_key, key->public_key_len);
		*outputlen = key->public_key_len;
		fido2_debug("FIDO2: sending client-first-message (%zu bytes, public key)\n",
					key->public_key_len);
		st->state = FIDO2_CLIENT_STATE_CHALLENGE_RECEIVED;
		return SASL_CONTINUE;
	}

	if (st->state == FIDO2_CLIENT_STATE_CHALLENGE_RECEIVED)
	{
		/* Parse challenge */
		p = (const uint8_t *) input;
		if (inputlen != FIDO2_CHALLENGE_MSG_LENGTH || p[0] != FIDO2_PROTOCOL_VERSION)
		{
			libpq_append_conn_error(st->conn, "invalid challenge");
			return SASL_FAILED;
		}
		memcpy(st->challenge, p + 1, FIDO2_CHALLENGE_LENGTH);
		st->options = p[33];
		fido2_debug("FIDO2: received server-challenge (version=%d, options=0x%02x)\n",
					p[0], st->options);

		/*
		 * Compute extended challenge = challenge || SHA256(rpId)
		 *
		 * This follows the OpenSSH sk-provider convention. The 64-byte extended
		 * challenge is passed to sk_sign() as raw binary data. The sk-provider
		 * (e.g., sk-usbhid.c) calls fido_assert_set_clientdata() which internally
		 * computes SHA256(extended_challenge) to produce the clientDataHash used
		 * in the FIDO2 assertion signature.
		 *
		 * This differs from WebAuthn, which uses SHA256(clientDataJSON) with a
		 * browser-provided JSON structure. The sk-provider API is designed for
		 * non-browser use cases like SSH and PostgreSQL authentication.
		 *
		 * Reference: OpenSSH PROTOCOL.u2f, lines 179-191
		 */
		ctx = pg_cryptohash_create(PG_SHA256);
		if (!ctx ||
			pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, (const uint8_t *) FIDO2_RP_ID, strlen(FIDO2_RP_ID)) < 0 ||
			pg_cryptohash_final(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(ctx);
			return SASL_FAILED;
		}
		pg_cryptohash_free(ctx);

		memcpy(ext, st->challenge, FIDO2_CHALLENGE_LENGTH);
		memcpy(ext + FIDO2_CHALLENGE_LENGTH, rp_hash, PG_SHA256_DIGEST_LENGTH);

		/* Sign */
		key = &st->keys[st->key_idx];
		flags = 0;
		if (st->options & FIDO2_OPT_REQUIRE_UP)
			flags |= SSH_SK_USER_PRESENCE_REQD;
		if (st->options & FIDO2_OPT_REQUIRE_UV)
			flags |= SSH_SK_USER_VERIFICATION_REQD;

		if (st->sign(SSH_SK_ECDSA, ext, sizeof(ext), FIDO2_RP_ID,
					 key->key_handle, key->key_handle_len, flags,
					 st->conn->fido2_pin, NULL, &sig) != 0 || !sig)
		{
			libpq_append_conn_error(st->conn, "signing failed");
			return SASL_FAILED;
		}

		/* Build response: flags(1) + counter(4) + signature(64) */
		out = malloc(FIDO2_ASSERTION_LENGTH);
		if (!out)
		{
			if (st->free_sign_response)
				st->free_sign_response(sig);
			else
			{
				free(sig->sig_r);
				free(sig->sig_s);
				free(sig);
			}
			return SASL_FAILED;
		}
		out[0] = sig->flags;
		{
			uint32_t counter_be = pg_hton32(sig->counter);
			memcpy(out + 1, &counter_be, sizeof(uint32_t));
		}

		/*
		 * ECDSA r and s components can be 1-32 bytes. Pad them to 32 bytes
		 * each, right-aligned (big-endian integer representation).
		 */
		memset(out + 5, 0, 64);
		if (sig->sig_r_len <= 32)
			memcpy(out + 5 + (32 - sig->sig_r_len), sig->sig_r, sig->sig_r_len);
		else
			memcpy(out + 5, sig->sig_r + (sig->sig_r_len - 32), 32);
		if (sig->sig_s_len <= 32)
			memcpy(out + 5 + 32 + (32 - sig->sig_s_len), sig->sig_s, sig->sig_s_len);
		else
			memcpy(out + 5 + 32, sig->sig_s + (sig->sig_s_len - 32), 32);

		fido2_debug("FIDO2: sending client-assertion (flags=0x%02x, counter=%u)\n",
					sig->flags, sig->counter);

		if (st->free_sign_response)
			st->free_sign_response(sig);
		else
		{
			free(sig->sig_r);
			free(sig->sig_s);
			free(sig);
		}

		*output = (char *) out;
		*outputlen = FIDO2_ASSERTION_LENGTH;
		st->state = FIDO2_CLIENT_STATE_FINISHED;
		return final ? SASL_COMPLETE : SASL_CONTINUE;
	}

	return final ? SASL_COMPLETE : SASL_FAILED;
}

static bool
fido2_channel_bound(void *state)
{
	return false;
}

static void
fido2_free(void *opaque)
{
	fe_fido2_state *st = (fe_fido2_state *) opaque;
	size_t i;

	if (!st)
		return;

	if (st->state != FIDO2_CLIENT_STATE_FINISHED && st->num_keys > 1)
		libpq_append_conn_error(st->conn,
								"HINT: Set fido2_credential connection parameter to select a different key");

	for (i = 0; i < st->num_keys; i++)
	{
		free(st->keys[i].public_key);
		free(st->keys[i].key_handle);
	}
	free(st->keys);
	if (st->handle)
		dlclose(st->handle);
	free(st);
}

#else /* WIN32 */

/*
 * FIDO2 is not supported on Windows - provide stub that reports error
 * rather than crashing with NULL function pointers.
 */
static void *
fido2_init_stub(PGconn *conn, const char *password, const char *mech)
{
	libpq_append_conn_error(conn, "FIDO2 authentication is not supported on Windows");
	return NULL;
}

const pg_fe_sasl_mech pg_fido2_mech = {fido2_init_stub, NULL, NULL, NULL};

#endif
