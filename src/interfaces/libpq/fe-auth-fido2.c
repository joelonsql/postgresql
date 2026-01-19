/*-------------------------------------------------------------------------
 *
 * fe-auth-fido2.c
 *	  Client-side implementation of FIDO2/WebAuthn SASL authentication.
 *
 * This implements the client-side SASL mechanism for FIDO2 authentication.
 * It uses a pluggable sk-provider interface (via dlopen) to communicate
 * with hardware security keys.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-fido2.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "port/pg_bswap.h"
#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/fido2.h"
#include "libpq/sk-provider.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

#include <string.h>

/*
 * Debug logging macro - enabled by PGFIDO2DEBUG environment variable
 */
#define FIDO2_DEBUG(msg, ...) \
	do { \
		if (getenv("PGFIDO2DEBUG")) \
			fprintf(stderr, "DEBUG FIDO2: " msg "\n", ##__VA_ARGS__); \
	} while(0)

/*
 * OpenSSH sk-api interface definitions for compatibility with
 * providers like macOS ssh-keychain.dylib
 */
#ifndef WIN32

/* OpenSSH sk-api version */
#define SSH_SK_VERSION_MAJOR		0x000a0000
#define SSH_SK_VERSION_MAJOR_MASK	0xffff0000

/* OpenSSH algorithm identifiers */
#define SSH_SK_ECDSA	0x00
#define SSH_SK_ED25519	0x01

/* OpenSSH flags */
#define SSH_SK_USER_PRESENCE_REQD		0x01
#define SSH_SK_USER_VERIFICATION_REQD	0x04

/* OpenSSH sign response structure */
struct sk_sign_response {
	uint8_t		flags;
	uint32_t	counter;
	uint8_t	   *sig_r;
	size_t		sig_r_len;
	uint8_t	   *sig_s;
	size_t		sig_s_len;
};

/* OpenSSH option structure */
struct sk_option {
	char	   *name;
	char	   *value;
	uint8_t		required;
};

/* OpenSSH function pointer types */
typedef uint32_t (*sk_api_version_fn)(void);
typedef int (*sk_sign_fn)(uint32_t alg, const uint8_t *data, size_t data_len,
						  const char *application,
						  const uint8_t *key_handle, size_t key_handle_len,
						  uint8_t flags, const char *pin,
						  struct sk_option **options,
						  struct sk_sign_response **sign_response);

/* OpenSSH resident key structures */
struct sk_enroll_response {
	uint8_t		flags;
	uint8_t	   *public_key;
	size_t		public_key_len;
	uint8_t	   *key_handle;
	size_t		key_handle_len;
	uint8_t	   *signature;
	size_t		signature_len;
	uint8_t	   *attestation_cert;
	size_t		attestation_cert_len;
	uint8_t	   *authdata;
	size_t		authdata_len;
};

struct sk_resident_key {
	uint32_t	alg;
	size_t		slot;
	char	   *application;
	struct sk_enroll_response key;
	uint8_t		flags;
	uint8_t	   *user_id;
	size_t		user_id_len;
};

typedef int (*sk_load_resident_keys_fn)(const char *pin,
										struct sk_option **options,
										struct sk_resident_key ***rks,
										size_t *nrks);

#endif /* WIN32 */

/*
 * Provider interface type
 */
typedef enum
{
	SK_PROVIDER_NONE,
	SK_PROVIDER_POSTGRESQL,		/* pg_sk_* interface */
	SK_PROVIDER_OPENSSH			/* sk_* interface (OpenSSH compatible) */
} sk_provider_type;

/*
 * State for FIDO2 client authentication
 */
typedef enum
{
	FE_FIDO2_INIT,
	FE_FIDO2_CHALLENGE_RECEIVED,
	FE_FIDO2_FINISHED
} fe_fido2_state_enum;

typedef struct
{
	fe_fido2_state_enum state;
	PGconn	   *conn;

	/* Loaded provider */
	pg_sk_provider provider;
	bool		provider_loaded;
	sk_provider_type provider_type;

#ifndef WIN32
	/* OpenSSH provider functions (when provider_type == SK_PROVIDER_OPENSSH) */
	sk_sign_fn	ssh_sign;
	sk_load_resident_keys_fn ssh_load_resident_keys;
#endif

	/* Challenge from server */
	uint8_t		challenge[FIDO2_CHALLENGE_LENGTH];
	char	   *rp_id;

	/* Credential IDs from server */
	uint8_t	  **credential_ids;
	int		   *credential_id_lens;
	int			num_credentials;

	/* Options from server */
	uint8_t		options;
} fe_fido2_state;

/* Forward declarations */
static void *fido2_init(PGconn *conn, const char *password, const char *mech);
static SASLStatus fido2_exchange(void *state, bool final,
								 char *input, int inputlen,
								 char **output, int *outputlen);
static bool fido2_channel_bound(void *state);
static void fido2_free(void *state);

static bool load_sk_provider(fe_fido2_state *state);
static bool parse_challenge(fe_fido2_state *state, const char *input, int inputlen);
static char *build_assertion(fe_fido2_state *state, int *outputlen);

/*
 * Mechanism declaration for FIDO2
 */
const pg_fe_sasl_mech pg_fido2_mech = {
	fido2_init,
	fido2_exchange,
	fido2_channel_bound,
	fido2_free
};

/*
 * Initialize FIDO2 client state
 */
static void *
fido2_init(PGconn *conn, const char *password, const char *mech)
{
	fe_fido2_state *state;

	state = malloc(sizeof(fe_fido2_state));
	if (!state)
		return NULL;

	memset(state, 0, sizeof(fe_fido2_state));
	state->state = FE_FIDO2_INIT;
	state->conn = conn;
	state->provider_loaded = false;

	/* Load the sk-provider library */
	if (!load_sk_provider(state))
	{
		free(state);
		return NULL;
	}

	return state;
}

/*
 * FIDO2 SASL exchange
 */
static SASLStatus
fido2_exchange(void *opaque, bool final,
			   char *input, int inputlen,
			   char **output, int *outputlen)
{
	fe_fido2_state *state = (fe_fido2_state *) opaque;

	*output = NULL;
	*outputlen = 0;

	FIDO2_DEBUG("exchange state=%d, inputlen=%d", state->state, inputlen);

	switch (state->state)
	{
		case FE_FIDO2_INIT:
			/*
			 * First call - we send an empty initial response.
			 * This triggers the server to send us the challenge.
			 */
			FIDO2_DEBUG("sending initial empty response");
			*output = malloc(1);
			if (!*output)
				return SASL_FAILED;
			(*output)[0] = '\0';
			*outputlen = 0;
			state->state = FE_FIDO2_CHALLENGE_RECEIVED;
			return SASL_CONTINUE;

		case FE_FIDO2_CHALLENGE_RECEIVED:
			/* Parse the challenge from server */
			FIDO2_DEBUG("received challenge (%d bytes)", inputlen);
			if (!parse_challenge(state, input, inputlen))
			{
				libpq_append_conn_error(state->conn,
										"failed to parse FIDO2 challenge");
				return SASL_FAILED;
			}

			/* Build and sign the assertion */
			*output = build_assertion(state, outputlen);
			if (*output == NULL)
			{
				/* Error already reported */
				return SASL_FAILED;
			}

			state->state = FE_FIDO2_FINISHED;

			if (final)
				return SASL_COMPLETE;
			return SASL_CONTINUE;

		case FE_FIDO2_FINISHED:
			if (final)
				return SASL_COMPLETE;
			/* Shouldn't happen */
			return SASL_FAILED;
	}

	return SASL_FAILED;
}

/*
 * FIDO2 doesn't support channel binding (yet)
 */
static bool
fido2_channel_bound(void *state)
{
	return false;
}

/*
 * Free FIDO2 state
 */
static void
fido2_free(void *opaque)
{
	fe_fido2_state *state = (fe_fido2_state *) opaque;
	int			i;

	if (!state)
		return;

	if (state->rp_id)
		free(state->rp_id);

	if (state->credential_ids)
	{
		for (i = 0; i < state->num_credentials; i++)
		{
			if (state->credential_ids[i])
				free(state->credential_ids[i]);
		}
		free(state->credential_ids);
	}

	if (state->credential_id_lens)
		free(state->credential_id_lens);

	if (state->provider_loaded)
		pg_sk_unload_provider(&state->provider);

	free(state);
}

/*
 * Load the security key provider library
 *
 * This function supports two provider interfaces:
 * 1. PostgreSQL native interface (pg_sk_* functions)
 * 2. OpenSSH sk-api interface (sk_* functions) for compatibility with
 *    providers like macOS ssh-keychain.dylib
 */
static bool
load_sk_provider(fe_fido2_state *state)
{
	const char *provider_path;

	/* Get provider path from connection parameter or environment */
	provider_path = state->conn->sk_provider;
	if (provider_path == NULL || provider_path[0] == '\0')
		provider_path = getenv("PGSKPROVIDER");

	if (provider_path == NULL || provider_path[0] == '\0')
	{
		libpq_append_conn_error(state->conn,
								"FIDO2 authentication requires sk_provider connection parameter or PGSKPROVIDER environment variable");
		return false;
	}

	FIDO2_DEBUG("loading provider from \"%s\"", provider_path);

#ifndef WIN32
	{
		void	   *handle;

		memset(&state->provider, 0, sizeof(pg_sk_provider));
		state->provider_type = SK_PROVIDER_NONE;

		handle = dlopen(provider_path, RTLD_NOW);
		if (!handle)
		{
			libpq_append_conn_error(state->conn,
									"failed to load sk-provider \"%s\": %s",
									provider_path, dlerror());
			return false;
		}

		/* Try PostgreSQL interface first */
		state->provider.api_version = (pg_sk_api_version_fn) dlsym(handle, "pg_sk_api_version");
		if (state->provider.api_version)
		{
			int			api_version;

			/* Load PostgreSQL interface functions */
			state->provider.sign = (pg_sk_sign_fn) dlsym(handle, "pg_sk_sign");
			state->provider.free_signature = (pg_sk_free_signature_fn) dlsym(handle, "pg_sk_free_signature");
			state->provider.strerror = (pg_sk_strerror_fn) dlsym(handle, "pg_sk_strerror");
			state->provider.enroll = (pg_sk_enroll_fn) dlsym(handle, "pg_sk_enroll");
			state->provider.free_pubkey = (pg_sk_free_pubkey_fn) dlsym(handle, "pg_sk_free_pubkey");

			if (!state->provider.sign || !state->provider.free_signature ||
				!state->provider.strerror)
			{
				libpq_append_conn_error(state->conn,
										"failed to load sk-provider \"%s\": provider missing required PostgreSQL sk functions",
										provider_path);
				dlclose(handle);
				return false;
			}

			/* Verify API version */
			api_version = state->provider.api_version();
			if (api_version != PG_SK_API_VERSION)
			{
				libpq_append_conn_error(state->conn,
										"failed to load sk-provider \"%s\": API version mismatch (expected %d, got %d)",
										provider_path, PG_SK_API_VERSION, api_version);
				dlclose(handle);
				return false;
			}

			state->provider_type = SK_PROVIDER_POSTGRESQL;
			FIDO2_DEBUG("detected provider type: PostgreSQL");
		}
		else
		{
			/* Try OpenSSH sk-api interface */
			sk_api_version_fn ssh_api_version;

			ssh_api_version = (sk_api_version_fn) dlsym(handle, "sk_api_version");
			if (!ssh_api_version)
			{
				libpq_append_conn_error(state->conn,
										"failed to load sk-provider \"%s\": provider has neither PostgreSQL (pg_sk_*) nor OpenSSH (sk_*) interface",
										provider_path);
				dlclose(handle);
				return false;
			}

			state->ssh_sign = (sk_sign_fn) dlsym(handle, "sk_sign");
			if (!state->ssh_sign)
			{
				libpq_append_conn_error(state->conn,
										"failed to load sk-provider \"%s\": provider missing required sk_sign function",
										provider_path);
				dlclose(handle);
				return false;
			}

			/* Load optional resident key loading function */
			state->ssh_load_resident_keys = (sk_load_resident_keys_fn)
				dlsym(handle, "sk_load_resident_keys");
			/* This function is optional - not all providers support it */

			/* Verify OpenSSH API version (major version check) */
			{
				uint32_t	ssh_version = ssh_api_version();

				if ((ssh_version & SSH_SK_VERSION_MAJOR_MASK) != SSH_SK_VERSION_MAJOR)
				{
					libpq_append_conn_error(state->conn,
											"failed to load sk-provider \"%s\": OpenSSH API version mismatch (expected 0x%08x, got 0x%08x)",
											provider_path, SSH_SK_VERSION_MAJOR, ssh_version);
					dlclose(handle);
					return false;
				}
			}

			state->provider_type = SK_PROVIDER_OPENSSH;
			FIDO2_DEBUG("detected provider type: OpenSSH");
		}

		FIDO2_DEBUG("provider API version verified");
		state->provider.handle = handle;
		state->provider_loaded = true;
		return true;
	}
#else
	libpq_append_conn_error(state->conn,
							"FIDO2 authentication not supported on this platform");
	return false;
#endif
}

/*
 * Parse challenge message from server
 *
 * Format:
 *   protocol_version: 1 byte
 *   challenge: 32 bytes
 *   rp_id_len: 2 bytes (big-endian)
 *   rp_id: rp_id_len bytes (null-terminated)
 *   credential_count: 1 byte
 *   For each credential:
 *     credential_id_len: 2 bytes (big-endian)
 *     credential_id: credential_id_len bytes
 *   options: 1 byte
 */
static bool
parse_challenge(fe_fido2_state *state, const char *input, int inputlen)
{
	const uint8_t *p = (const uint8_t *) input;
	const uint8_t *end = p + inputlen;
	uint8_t		version;
	uint16_t	rp_id_len;
	uint8_t		cred_count;
	int			i;

	/* Protocol version */
	if (end - p < 1)
		return false;
	version = *p++;
	FIDO2_DEBUG("protocol version: %d", version);
	if (version != FIDO2_PROTOCOL_VERSION)
	{
		libpq_append_conn_error(state->conn,
								"unsupported FIDO2 protocol version: %d",
								version);
		return false;
	}

	/* Challenge */
	if (end - p < FIDO2_CHALLENGE_LENGTH)
		return false;
	memcpy(state->challenge, p, FIDO2_CHALLENGE_LENGTH);
	p += FIDO2_CHALLENGE_LENGTH;

	/* Relying party ID */
	if (end - p < 2)
		return false;
	memcpy(&rp_id_len, p, 2);
	rp_id_len = pg_ntoh16(rp_id_len);
	p += 2;

	if (end - p < rp_id_len)
		return false;
	state->rp_id = malloc(rp_id_len);
	if (!state->rp_id)
		return false;
	memcpy(state->rp_id, p, rp_id_len);
	p += rp_id_len;
	FIDO2_DEBUG("rp_id: \"%s\"", state->rp_id);

	/* Number of credentials */
	if (end - p < 1)
		return false;
	cred_count = *p++;
	state->num_credentials = cred_count;
	FIDO2_DEBUG("num_credentials: %d", state->num_credentials);

	/* Credential IDs */
	if (cred_count > 0)
	{
		state->credential_ids = malloc(sizeof(uint8_t *) * cred_count);
		state->credential_id_lens = malloc(sizeof(int) * cred_count);
		if (!state->credential_ids || !state->credential_id_lens)
			return false;

		for (i = 0; i < cred_count; i++)
		{
			uint16_t	cred_len;

			if (end - p < 2)
				return false;
			memcpy(&cred_len, p, 2);
			cred_len = pg_ntoh16(cred_len);
			p += 2;

			if (end - p < cred_len)
				return false;

			state->credential_ids[i] = malloc(cred_len);
			if (!state->credential_ids[i])
				return false;
			memcpy(state->credential_ids[i], p, cred_len);
			state->credential_id_lens[i] = cred_len;
			p += cred_len;
		}
	}

	/* Options */
	if (end - p < 1)
		return false;
	state->options = *p++;
	FIDO2_DEBUG("options: 0x%02x", state->options);

	return true;
}

/*
 * Wrapper function to call the appropriate signing function based on
 * provider type. Handles conversion between OpenSSH and PostgreSQL
 * signature formats.
 *
 * For OpenSSH providers:
 * - Converts flags from PostgreSQL to OpenSSH format
 * - Calls sk_sign() with individual parameters
 * - Converts signature from separate R/S to concatenated R||S
 * - Allocates signature buffer ourselves (caller frees with free())
 *
 * For PostgreSQL providers:
 * - Calls pg_sk_sign() directly with struct parameters
 * - Provider allocates signature (caller frees with provider->free_signature())
 *
 * Returns PG_SK_ERR_SUCCESS on success, error code on failure.
 */
static int
call_provider_sign(fe_fido2_state *state, pg_sk_sign_params *params,
				   pg_sk_signature *sig)
{
	if (state->provider_type == SK_PROVIDER_POSTGRESQL)
	{
		/* Direct call to PostgreSQL interface */
		return state->provider.sign(params, sig);
	}
#ifndef WIN32
	else if (state->provider_type == SK_PROVIDER_OPENSSH)
	{
		/* Call OpenSSH interface with conversion */
		struct sk_sign_response *ssh_sig = NULL;
		uint8_t		ssh_flags = 0;
		uint32_t	alg = SSH_SK_ECDSA;	/* Default to ECDSA */
		int			result;

		/* Variables for extended challenge computation */
		uint8_t		extended_challenge[FIDO2_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH];
		uint8_t		rp_id_hash[PG_SHA256_DIGEST_LENGTH];
		pg_cryptohash_ctx *sha256_ctx;

		/* Convert flags from PostgreSQL to OpenSSH format */
		if (params->flags & PG_SK_FLAG_REQUIRE_UP)
			ssh_flags |= SSH_SK_USER_PRESENCE_REQD;
		if (params->flags & PG_SK_FLAG_REQUIRE_UV)
			ssh_flags |= SSH_SK_USER_VERIFICATION_REQD;

		/*
		 * Build extended challenge for OpenSSH middleware compatibility.
		 * The server computes clientDataHash as SHA256(challenge || rpIdHash).
		 * The OpenSSH middleware computes SHA256(data_passed_to_sk_sign).
		 * By passing (challenge || rpIdHash), the hashes will match.
		 */

		/* Compute rpIdHash = SHA256(application) */
		sha256_ctx = pg_cryptohash_create(PG_SHA256);
		if (!sha256_ctx)
			return PG_SK_ERR_NO_MEMORY;

		if (pg_cryptohash_init(sha256_ctx) < 0 ||
			pg_cryptohash_update(sha256_ctx, (const uint8_t *) params->application,
								 strlen(params->application)) < 0 ||
			pg_cryptohash_final(sha256_ctx, rp_id_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(sha256_ctx);
			return PG_SK_ERR_GENERAL;
		}
		pg_cryptohash_free(sha256_ctx);

		/* Build extended challenge: challenge || rpIdHash */
		memcpy(extended_challenge, params->challenge, FIDO2_CHALLENGE_LENGTH);
		memcpy(extended_challenge + FIDO2_CHALLENGE_LENGTH, rp_id_hash,
			   PG_SHA256_DIGEST_LENGTH);
		FIDO2_DEBUG("computing extended challenge (64 bytes)");

		/* Call OpenSSH sk_sign with extended challenge */
		FIDO2_DEBUG("calling sk_sign with application=\"%s\"", params->application);
		result = state->ssh_sign(alg,
								 extended_challenge,
								 FIDO2_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH,
								 params->application,
								 params->key_handle, params->key_handle_len,
								 ssh_flags,
								 params->pin,
								 NULL,	/* options */
								 &ssh_sig);

		if (result != 0 || !ssh_sig)
			return PG_SK_ERR_GENERAL;

		/* Convert signature from separate R/S to concatenated R||S */
		sig->flags = ssh_sig->flags;
		sig->counter = ssh_sig->counter;
		sig->signature_len = ssh_sig->sig_r_len + ssh_sig->sig_s_len;
		sig->signature = malloc(sig->signature_len);
		if (!sig->signature)
		{
			free(ssh_sig->sig_r);
			free(ssh_sig->sig_s);
			free(ssh_sig);
			return PG_SK_ERR_NO_MEMORY;
		}

		memcpy(sig->signature, ssh_sig->sig_r, ssh_sig->sig_r_len);
		memcpy(sig->signature + ssh_sig->sig_r_len, ssh_sig->sig_s,
			   ssh_sig->sig_s_len);

		/* Free OpenSSH response */
		free(ssh_sig->sig_r);
		free(ssh_sig->sig_s);
		free(ssh_sig);

		return PG_SK_ERR_SUCCESS;
	}
#endif

	return PG_SK_ERR_GENERAL;
}

/*
 * Free signature based on provider type
 */
static void
free_provider_signature(fe_fido2_state *state, pg_sk_signature *sig)
{
	if (sig->signature == NULL)
		return;

	if (state->provider_type == SK_PROVIDER_POSTGRESQL)
	{
		/* Provider allocated; use provider's free function */
		state->provider.free_signature(sig);
	}
	else
	{
		/* We allocated in call_provider_sign; use free() directly */
		free(sig->signature);
		sig->signature = NULL;
	}
}

/*
 * Get error string for a signing error
 */
static const char *
get_sign_error_string(fe_fido2_state *state, int error)
{
	if (state->provider_type == SK_PROVIDER_POSTGRESQL &&
		state->provider.strerror)
	{
		return state->provider.strerror(error);
	}

	/* Generic error strings for OpenSSH providers */
	switch (error)
	{
		case PG_SK_ERR_SUCCESS:
			return "success";
		case PG_SK_ERR_GENERAL:
			return "general error";
		case PG_SK_ERR_NO_DEVICE:
			return "no device found";
		case PG_SK_ERR_TIMEOUT:
			return "operation timed out";
		case PG_SK_ERR_NO_CREDENTIALS:
			return "no matching credential";
		case PG_SK_ERR_PIN_REQUIRED:
			return "PIN required";
		case PG_SK_ERR_PIN_INVALID:
			return "invalid PIN";
		case PG_SK_ERR_UNSUPPORTED:
			return "unsupported operation";
		case PG_SK_ERR_NO_MEMORY:
			return "out of memory";
		case PG_SK_ERR_CANCELLED:
			return "operation cancelled";
		default:
			return "unknown error";
	}
}

/*
 * Build assertion response by signing with the security key
 *
 * Returns malloc'd buffer on success, NULL on failure.
 *
 * Format:
 *   credential_id_len: 2 bytes (big-endian)
 *   credential_id: credential_id_len bytes
 *   authenticator_data_len: 2 bytes (big-endian)
 *   authenticator_data: authenticator_data_len bytes
 *   signature_len: 2 bytes (big-endian)
 *   signature: signature_len bytes
 */
static char *
build_assertion(fe_fido2_state *state, int *outputlen)
{
	pg_sk_sign_params params;
	pg_sk_signature sig;
	int			result;
	char	   *output;
	size_t		total_len;
	uint8_t	   *p;
	uint16_t	len16;
	uint8_t		auth_data[37];	/* rpIdHash(32) + flags(1) + counter(4) */
	pg_cryptohash_ctx *sha256_ctx;
	int			i;
#ifndef WIN32
	/* Resident key support variables */
	struct sk_resident_key **rks = NULL;
	size_t		nrks = 0;
	uint8_t	   *resident_key_handle = NULL;
	size_t		resident_key_handle_len = 0;
#endif

	/* Find a matching credential */
	memset(&params, 0, sizeof(params));
	memset(&sig, 0, sizeof(sig));

#ifndef WIN32
	/*
	 * For OpenSSH providers, try to load resident keys to get the actual
	 * key_handle. The server sends the RP ID (application) as credential_id,
	 * but the middleware needs the real key handle stored on the security key.
	 */
	if (state->provider_type == SK_PROVIDER_OPENSSH &&
		state->ssh_load_resident_keys)
	{
		int			rk_result;

		/* Load resident keys from security key */
		FIDO2_DEBUG("attempting resident key load");
		rk_result = state->ssh_load_resident_keys(
			state->conn->fido2_pin,		/* PIN */
			NULL,						/* options */
			&rks,
			&nrks);

		if (rk_result == 0 && nrks > 0)
		{
			FIDO2_DEBUG("found %zu resident keys", nrks);
			/* Find key matching our rp_id (application) */
			for (size_t j = 0; j < nrks; j++)
			{
				if (rks[j]->application &&
					strcmp(rks[j]->application, state->rp_id) == 0)
				{
					/* Found matching key - use its key_handle */
					resident_key_handle = rks[j]->key.key_handle;
					resident_key_handle_len = rks[j]->key.key_handle_len;
					break;
				}
			}
		}
	}
#endif

	/* Try each credential ID */
	for (i = 0; i < state->num_credentials; i++)
	{
		FIDO2_DEBUG("trying credential %d/%d", i + 1, state->num_credentials);
		params.application = state->rp_id;
		params.challenge = state->challenge;
		params.challenge_len = FIDO2_CHALLENGE_LENGTH;
		params.device = state->conn->fido2_device;
		params.pin = state->conn->fido2_pin;
		params.flags = 0;

#ifndef WIN32
		/*
		 * If we found a resident key matching the rp_id, use its key_handle
		 * instead of the server-provided credential_id.
		 */
		if (resident_key_handle != NULL)
		{
			params.key_handle = resident_key_handle;
			params.key_handle_len = resident_key_handle_len;
		}
		else
#endif
		{
			params.key_handle = state->credential_ids[i];
			params.key_handle_len = state->credential_id_lens[i];
		}

		if (state->options & FIDO2_OPT_REQUIRE_UP)
			params.flags |= PG_SK_FLAG_REQUIRE_UP;
		if (state->options & FIDO2_OPT_REQUIRE_UV)
			params.flags |= PG_SK_FLAG_REQUIRE_UV;

		FIDO2_DEBUG("signing with key_handle_len=%zu", params.key_handle_len);
		result = call_provider_sign(state, &params, &sig);
		FIDO2_DEBUG("sign result: %d", result);
		if (result == PG_SK_ERR_SUCCESS)
			break;

		/* Try next credential on no-match error */
		if (result != PG_SK_ERR_NO_CREDENTIALS)
		{
			libpq_append_conn_error(state->conn,
									"FIDO2 signing failed: %s",
									get_sign_error_string(state, result));
			return NULL;
		}
	}

	if (result != PG_SK_ERR_SUCCESS)
	{
		libpq_append_conn_error(state->conn,
								"no matching FIDO2 credential found");
		return NULL;
	}

	/*
	 * Build authenticator data:
	 *   rpIdHash: SHA-256 of rp_id (32 bytes)
	 *   flags: 1 byte
	 *   signCount: 4 bytes (big-endian)
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx)
	{
		free_provider_signature(state, &sig);
		libpq_append_conn_error(state->conn, "out of memory");
		return NULL;
	}
	if (pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, (const uint8_t *) state->rp_id,
							 strlen(state->rp_id)) < 0 ||
		pg_cryptohash_final(sha256_ctx, auth_data, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		pg_cryptohash_free(sha256_ctx);
		free_provider_signature(state, &sig);
		libpq_append_conn_error(state->conn, "SHA-256 computation failed");
		return NULL;
	}
	pg_cryptohash_free(sha256_ctx);

	auth_data[32] = sig.flags;
	auth_data[33] = (sig.counter >> 24) & 0xff;
	auth_data[34] = (sig.counter >> 16) & 0xff;
	auth_data[35] = (sig.counter >> 8) & 0xff;
	auth_data[36] = sig.counter & 0xff;

	/*
	 * Calculate total output length.
	 * Note: The credential_id in the response should be the original
	 * credential_id from the server (e.g., "ssh:"), NOT the key_handle
	 * used for signing (which may differ for OpenSSH middleware).
	 */
	total_len = 2 + state->credential_id_lens[i] +	/* credential_id */
		2 + 37 +				/* authenticator_data */
		2 + sig.signature_len;	/* signature */

	output = malloc(total_len);
	if (!output)
	{
		free_provider_signature(state, &sig);
		return NULL;
	}

	p = (uint8_t *) output;

	/* Credential ID - use the original credential_id, not key_handle */
	len16 = pg_hton16(state->credential_id_lens[i]);
	memcpy(p, &len16, 2);
	p += 2;
	memcpy(p, state->credential_ids[i], state->credential_id_lens[i]);
	p += state->credential_id_lens[i];

	/* Authenticator data */
	len16 = pg_hton16(37);
	memcpy(p, &len16, 2);
	p += 2;
	memcpy(p, auth_data, 37);
	p += 37;

	/* Signature */
	len16 = pg_hton16(sig.signature_len);
	memcpy(p, &len16, 2);
	p += 2;
	memcpy(p, sig.signature, sig.signature_len);

	free_provider_signature(state, &sig);

	FIDO2_DEBUG("built assertion: credential_id=%d, auth_data=37, sig=%zu bytes",
				state->credential_id_lens[i], sig.signature_len);

	*outputlen = total_len;
	return output;
}

/*
 * Load a security key provider library
 */
bool
pg_sk_load_provider(const char *path, pg_sk_provider *provider, char **errmsg)
{
#ifndef WIN32
	void	   *handle;
	int			api_version;

	*errmsg = NULL;
	memset(provider, 0, sizeof(pg_sk_provider));

	handle = dlopen(path, RTLD_NOW);
	if (!handle)
	{
		*errmsg = strdup(dlerror());
		return false;
	}

	/* Load required functions */
	provider->api_version = (pg_sk_api_version_fn) dlsym(handle, "pg_sk_api_version");
	provider->enroll = (pg_sk_enroll_fn) dlsym(handle, "pg_sk_enroll");
	provider->sign = (pg_sk_sign_fn) dlsym(handle, "pg_sk_sign");
	provider->free_pubkey = (pg_sk_free_pubkey_fn) dlsym(handle, "pg_sk_free_pubkey");
	provider->free_signature = (pg_sk_free_signature_fn) dlsym(handle, "pg_sk_free_signature");
	provider->strerror = (pg_sk_strerror_fn) dlsym(handle, "pg_sk_strerror");

	/* Validate required functions */
	if (!provider->api_version || !provider->sign ||
		!provider->free_signature || !provider->strerror)
	{
		*errmsg = strdup("provider missing required functions");
		dlclose(handle);
		return false;
	}

	/* Check API version */
	api_version = provider->api_version();
	if (api_version != PG_SK_API_VERSION)
	{
		char		buf[128];

		snprintf(buf, sizeof(buf),
				 "provider API version mismatch (expected %d, got %d)",
				 PG_SK_API_VERSION, api_version);
		*errmsg = strdup(buf);
		dlclose(handle);
		return false;
	}

	provider->handle = handle;
	return true;
#else
	*errmsg = strdup("dlopen not available on this platform");
	return false;
#endif
}

/*
 * Unload a security key provider
 */
void
pg_sk_unload_provider(pg_sk_provider *provider)
{
#ifndef WIN32
	if (provider->handle)
	{
		dlclose(provider->handle);
		provider->handle = NULL;
	}
#endif
}
