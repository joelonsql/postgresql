/*-------------------------------------------------------------------------
 *
 * fe-auth-skauth.c
 *	  Client-side implementation of ssh-sk SASL authentication.
 *
 * This implements the client-side SASL mechanism for ssh-sk authentication.
 * It uses a pluggable sk-provider interface (via dlopen) to communicate
 * with hardware security keys.
 *
 * The protocol follows the SSH model: client proposes a public key from
 * a resident credential, and the server accepts or rejects it. If rejected,
 * the client can restart SASL with the next available key.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-skauth.c
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
#include "libpq/skauth.h"
#include "libpq/sk-provider.h"
#include "common/skauth-pubkey.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

#include <string.h>

/*
 * Environment variable to select which resident key to use (0-based index)
 */
#define PGSSHSKKEY_ENV "PGSSHSKKEY"

/*
 * Debug logging macro - enabled by PGSKAUTHDEBUG environment variable
 */
#define SKAUTH_DEBUG(msg, ...) \
	do { \
		if (getenv("PGSKAUTHDEBUG")) \
			fprintf(stderr, "DEBUG skauth: " msg "\n", ##__VA_ARGS__); \
	} while(0)

/*
 * Format a public key fingerprint for debug output.
 * Returns a pointer to a static buffer (not thread-safe, for debug only).
 */
static const char *
format_pubkey_fingerprint(const uint8_t *pubkey, size_t pubkey_len)
{
	static char buf[64];
	size_t		i;
	size_t		pos = 0;

	if (pubkey == NULL || pubkey_len < 8)
		return "(invalid key)";

	/* Show first 8 bytes as hex */
	for (i = 0; i < 8 && pos < sizeof(buf) - 3; i++)
	{
		snprintf(buf + pos, sizeof(buf) - pos, "%02x", pubkey[i]);
		pos += 2;
	}
	snprintf(buf + pos, sizeof(buf) - pos, "...");

	return buf;
}

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
 * State for ssh-sk client authentication
 */
typedef enum
{
	FE_SKAUTH_INIT,
	FE_SKAUTH_CHALLENGE_RECEIVED,
	FE_SKAUTH_FINISHED
} fe_skauth_state_enum;

/*
 * Information about a resident key discovered on the authenticator
 */
typedef struct
{
	uint8_t	   *public_key;			/* Uncompressed EC point (65 bytes) */
	size_t		public_key_len;
	uint8_t	   *key_handle;			/* Credential ID / key handle */
	size_t		key_handle_len;
} fe_skauth_resident_key;

typedef struct
{
	fe_skauth_state_enum state;
	PGconn	   *conn;

	/* Loaded provider */
	pg_sk_provider provider;
	bool		provider_loaded;
	sk_provider_type provider_type;

#ifndef WIN32
	/* OpenSSH provider functions (when provider_type == SK_PROVIDER_OPENSSH) */
	sk_sign_fn	ssh_sign;
	sk_load_resident_keys_fn ssh_load_resident_keys;

	/* Resident keys discovered for "ssh:" */
	fe_skauth_resident_key *resident_keys;
	size_t		num_resident_keys;
	size_t		current_key_index;	/* Which key we're currently trying */
#endif

	/* Challenge from server */
	uint8_t		challenge[SKAUTH_CHALLENGE_LENGTH];

	/* Options from server */
	uint8_t		options;
} fe_skauth_state;

/* Forward declarations */
static void *skauth_init(PGconn *conn, const char *password, const char *mech);
static SASLStatus skauth_exchange(void *state, bool final,
								 char *input, int inputlen,
								 char **output, int *outputlen);
static bool skauth_channel_bound(void *state);
static void skauth_free(void *state);

static bool load_sk_provider(fe_skauth_state *state);
static bool load_resident_keys(fe_skauth_state *state);
static bool parse_challenge(fe_skauth_state *state, const char *input, int inputlen);
static char *build_assertion(fe_skauth_state *state, int *outputlen);

/*
 * Mechanism declaration for sk-provider
 */
const pg_fe_sasl_mech pg_skauth_mech = {
	skauth_init,
	skauth_exchange,
	skauth_channel_bound,
	skauth_free
};

/*
 * Initialize ssh-sk client state
 */
static void *
skauth_init(PGconn *conn, const char *password, const char *mech)
{
	fe_skauth_state *state;

	state = malloc(sizeof(fe_skauth_state));
	if (!state)
		return NULL;

	memset(state, 0, sizeof(fe_skauth_state));
	state->state = FE_SKAUTH_INIT;
	state->conn = conn;
	state->provider_loaded = false;
#ifndef WIN32
	state->resident_keys = NULL;
	state->num_resident_keys = 0;
	state->current_key_index = 0;
#endif

	/* Load the sk-provider library */
	if (!load_sk_provider(state))
	{
		free(state);
		return NULL;
	}

	/* Load resident keys for "ssh:" */
	if (!load_resident_keys(state))
	{
		skauth_free(state);
		return NULL;
	}

	return state;
}

/*
 * ssh-sk SASL exchange
 */
static SASLStatus
skauth_exchange(void *opaque, bool final,
			   char *input, int inputlen,
			   char **output, int *outputlen)
{
	fe_skauth_state *state = (fe_skauth_state *) opaque;

	*output = NULL;
	*outputlen = 0;

	SKAUTH_DEBUG("exchange state=%d, inputlen=%d", state->state, inputlen);

	switch (state->state)
	{
		case FE_SKAUTH_INIT:
#ifndef WIN32
			/*
			 * First call - send the public key from current resident key.
			 * Server will respond with challenge if it recognizes the key.
			 */
			if (state->current_key_index >= state->num_resident_keys)
			{
				libpq_append_conn_error(state->conn,
										"no ssh-sk credentials available");
				return SASL_FAILED;
			}

			{
				fe_skauth_resident_key *key = &state->resident_keys[state->current_key_index];

				SKAUTH_DEBUG("sending public key (%zu bytes) for key index %zu: %s",
							 key->public_key_len, state->current_key_index,
							 format_pubkey_fingerprint(key->public_key, key->public_key_len));

				*output = malloc(key->public_key_len);
				if (!*output)
					return SASL_FAILED;

				memcpy(*output, key->public_key, key->public_key_len);
				*outputlen = key->public_key_len;
			}

			state->state = FE_SKAUTH_CHALLENGE_RECEIVED;
			return SASL_CONTINUE;
#else
			libpq_append_conn_error(state->conn,
									"ssh-sk authentication not supported on this platform");
			return SASL_FAILED;
#endif

		case FE_SKAUTH_CHALLENGE_RECEIVED:
			/* Parse the challenge from server */
			SKAUTH_DEBUG("received challenge (%d bytes)", inputlen);
			if (!parse_challenge(state, input, inputlen))
			{
				libpq_append_conn_error(state->conn,
										"failed to parse ssh-sk challenge");
				return SASL_FAILED;
			}

			/* Build and sign the assertion */
			*output = build_assertion(state, outputlen);
			if (*output == NULL)
			{
				/* Error already reported */
				return SASL_FAILED;
			}

			state->state = FE_SKAUTH_FINISHED;

			if (final)
				return SASL_COMPLETE;
			return SASL_CONTINUE;

		case FE_SKAUTH_FINISHED:
			if (final)
				return SASL_COMPLETE;
			/* Shouldn't happen */
			return SASL_FAILED;
	}

	return SASL_FAILED;
}

/*
 * sk-provider doesn't support channel binding (yet)
 */
static bool
skauth_channel_bound(void *state)
{
	return false;
}

/*
 * Free ssh-sk state
 */
static void
skauth_free(void *opaque)
{
	fe_skauth_state *state = (fe_skauth_state *) opaque;

	if (!state)
		return;

#ifndef WIN32
	/*
	 * If authentication didn't complete and there are multiple keys available,
	 * show a hint to the user about how to select a different key.
	 * This helps when the first key isn't registered but another one is.
	 */
	if (state->state != FE_SKAUTH_FINISHED && state->num_resident_keys > 1)
	{
		fprintf(stderr,
				"HINT: You have %zu ssh keys available. "
				"Set PGSSHSKKEY to a key from 'ssh-add -L' to select one.\n",
				state->num_resident_keys);
	}

	/* Free resident keys */
	if (state->resident_keys)
	{
		for (size_t i = 0; i < state->num_resident_keys; i++)
		{
			free(state->resident_keys[i].public_key);
			free(state->resident_keys[i].key_handle);
		}
		free(state->resident_keys);
	}
#endif

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
load_sk_provider(fe_skauth_state *state)
{
	const char *provider_path;

	/* Get provider path from connection parameter or environment */
	provider_path = state->conn->sk_provider;
	if (provider_path == NULL || provider_path[0] == '\0')
		provider_path = getenv("PGSKPROVIDER");

	if (provider_path == NULL || provider_path[0] == '\0')
	{
		libpq_append_conn_error(state->conn,
								"sk-provider authentication requires sk_provider connection parameter or PGSKPROVIDER environment variable");
		return false;
	}

	SKAUTH_DEBUG("loading provider from \"%s\"", provider_path);

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
			SKAUTH_DEBUG("detected provider type: PostgreSQL");
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
			SKAUTH_DEBUG("detected provider type: OpenSSH");
		}

		SKAUTH_DEBUG("provider API version verified");
		state->provider.handle = handle;
		state->provider_loaded = true;
		return true;
	}
#else
	libpq_append_conn_error(state->conn,
							"ssh-sk authentication not supported on this platform");
	return false;
#endif
}

/*
 * Load resident keys from the security key for the "ssh:" application.
 *
 * This discovers all resident credentials stored on the authenticator
 * that were created with the "ssh:" RP ID (standard for SSH security keys).
 *
 * The PGSSHSKKEY environment variable can be set to select which key to use
 * (0-based index). If not set, the first key (index 0) is used.
 */
static bool
load_resident_keys(fe_skauth_state *state)
{
#ifndef WIN32
	if (state->provider_type == SK_PROVIDER_OPENSSH &&
		state->ssh_load_resident_keys)
	{
		struct sk_resident_key **rks = NULL;
		size_t		nrks = 0;
		int			result;
		size_t		match_count = 0;
		const char *key_str;

		SKAUTH_DEBUG("loading resident keys for application \"%s\"", SKAUTH_RP_ID);
		result = state->ssh_load_resident_keys(
			state->conn->skauth_pin,
			NULL,	/* options */
			&rks,
			&nrks);

		if (result != 0)
		{
			libpq_append_conn_error(state->conn,
									"failed to load resident keys from security key");
			return false;
		}

		SKAUTH_DEBUG("found %zu total resident keys", nrks);

		/* Count keys matching "ssh:" */
		for (size_t i = 0; i < nrks; i++)
		{
			if (rks[i]->application &&
				strcmp(rks[i]->application, SKAUTH_RP_ID) == 0)
				match_count++;
		}

		if (match_count == 0)
		{
			libpq_append_conn_error(state->conn,
									"no resident keys found for \"%s\" on security key",
									SKAUTH_RP_ID);
			/* Free the OpenSSH response */
			for (size_t i = 0; i < nrks; i++)
			{
				free(rks[i]->application);
				free(rks[i]->key.public_key);
				free(rks[i]->key.key_handle);
				free(rks[i]->user_id);
				free(rks[i]);
			}
			free(rks);
			return false;
		}

		/* Allocate our resident key array */
		state->resident_keys = malloc(sizeof(fe_skauth_resident_key) * match_count);
		if (!state->resident_keys)
		{
			for (size_t i = 0; i < nrks; i++)
			{
				free(rks[i]->application);
				free(rks[i]->key.public_key);
				free(rks[i]->key.key_handle);
				free(rks[i]->user_id);
				free(rks[i]);
			}
			free(rks);
			return false;
		}

		/* Copy matching keys */
		state->num_resident_keys = 0;
		for (size_t i = 0; i < nrks; i++)
		{
			if (rks[i]->application &&
				strcmp(rks[i]->application, SKAUTH_RP_ID) == 0)
			{
				fe_skauth_resident_key *key = &state->resident_keys[state->num_resident_keys];

				/* Copy public key */
				key->public_key_len = rks[i]->key.public_key_len;
				key->public_key = malloc(key->public_key_len);
				if (key->public_key)
					memcpy(key->public_key, rks[i]->key.public_key, key->public_key_len);

				/* Copy key handle */
				key->key_handle_len = rks[i]->key.key_handle_len;
				key->key_handle = malloc(key->key_handle_len);
				if (key->key_handle)
					memcpy(key->key_handle, rks[i]->key.key_handle, key->key_handle_len);

				SKAUTH_DEBUG("loaded resident key %zu: fingerprint=%s",
							 state->num_resident_keys,
							 format_pubkey_fingerprint(key->public_key, key->public_key_len));

				state->num_resident_keys++;
			}

			/* Free the OpenSSH struct */
			free(rks[i]->application);
			free(rks[i]->key.public_key);
			free(rks[i]->key.key_handle);
			free(rks[i]->user_id);
			free(rks[i]);
		}
		free(rks);

		SKAUTH_DEBUG("loaded %zu resident keys for \"%s\"",
					 state->num_resident_keys, SKAUTH_RP_ID);

		/*
		 * Check PGSSHSKKEY environment variable to select which key to use.
		 * Value should be a full SSH key string from 'ssh-add -L'.
		 */
		key_str = getenv(PGSSHSKKEY_ENV);
		if (key_str != NULL && key_str[0] != '\0')
		{
			SkauthParsedPubkey parsed;
			char	   *parse_errmsg = NULL;
			bool		found = false;

			/* Parse as SSH key string (ssh-add -L format) */
			if (!skauth_parse_openssh_pubkey(key_str, &parsed, &parse_errmsg))
			{
				libpq_append_conn_error(state->conn,
										"invalid %s: %s. "
										"Expected format from 'ssh-add -L'.",
										PGSSHSKKEY_ENV,
										parse_errmsg ? parse_errmsg : "could not parse SSH key string");
				free(parse_errmsg);
				return false;
			}

			/* Find matching resident key by public key comparison */
			for (size_t i = 0; i < state->num_resident_keys; i++)
			{
				if (state->resident_keys[i].public_key_len == parsed.public_key_len &&
					memcmp(state->resident_keys[i].public_key,
						   parsed.public_key,
						   parsed.public_key_len) == 0)
				{
					state->current_key_index = i;
					found = true;
					break;
				}
			}

			skauth_free_parsed_pubkey(&parsed);

			if (!found)
			{
				libpq_append_conn_error(state->conn,
										"specified SSH key not found on authenticator. "
										"Run 'ssh-add -L' to see available keys.");
				return false;
			}

			SKAUTH_DEBUG("using key index %zu (from %s)",
						 state->current_key_index, PGSSHSKKEY_ENV);
		}
		else
		{
			/* Default to first key */
			state->current_key_index = 0;

			/*
			 * If there are multiple keys and auth fails, the user needs to
			 * know about PGSSHSKKEY. We'll remind them of this in the debug
			 * output when we send the key.
			 */
		}

		return true;
	}

	/* For PostgreSQL providers, we don't need resident key discovery */
	if (state->provider_type == SK_PROVIDER_POSTGRESQL)
	{
		/* PostgreSQL providers handle key discovery internally */
		return true;
	}

	libpq_append_conn_error(state->conn,
							"sk-provider does not support resident key discovery");
	return false;
#else
	libpq_append_conn_error(state->conn,
							"ssh-sk authentication not supported on this platform");
	return false;
#endif
}

/*
 * Parse challenge message from server
 *
 * Simplified format:
 *   protocol_version: 1 byte
 *   challenge: 32 bytes
 *   options: 1 byte
 *
 * Total: 34 bytes
 */
static bool
parse_challenge(fe_skauth_state *state, const char *input, int inputlen)
{
	const uint8_t *p = (const uint8_t *) input;
	const uint8_t *end = p + inputlen;
	uint8_t		version;

	/* Protocol version */
	if (end - p < 1)
		return false;
	version = *p++;
	SKAUTH_DEBUG("protocol version: %d", version);
	if (version != SKAUTH_PROTOCOL_VERSION)
	{
		libpq_append_conn_error(state->conn,
								"unsupported ssh-sk protocol version: %d",
								version);
		return false;
	}

	/* Challenge */
	if (end - p < SKAUTH_CHALLENGE_LENGTH)
		return false;
	memcpy(state->challenge, p, SKAUTH_CHALLENGE_LENGTH);
	p += SKAUTH_CHALLENGE_LENGTH;

	/* Options */
	if (end - p < 1)
		return false;
	state->options = *p++;
	SKAUTH_DEBUG("options: 0x%02x", state->options);

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
call_provider_sign(fe_skauth_state *state, pg_sk_sign_params *params,
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
		uint8_t		extended_challenge[SKAUTH_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH];
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
		memcpy(extended_challenge, params->challenge, SKAUTH_CHALLENGE_LENGTH);
		memcpy(extended_challenge + SKAUTH_CHALLENGE_LENGTH, rp_id_hash,
			   PG_SHA256_DIGEST_LENGTH);
		SKAUTH_DEBUG("computing extended challenge (64 bytes)");

		/* Call OpenSSH sk_sign with extended challenge */
		SKAUTH_DEBUG("calling sk_sign with application=\"%s\"", params->application);
		result = state->ssh_sign(alg,
								 extended_challenge,
								 SKAUTH_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH,
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
free_provider_signature(fe_skauth_state *state, pg_sk_signature *sig)
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
get_sign_error_string(fe_skauth_state *state, int error)
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
 * Simplified format:
 *   sig_flags: 1 byte
 *   counter: 4 bytes (big-endian)
 *   signature: 64 bytes (R || S)
 *
 * Total: 69 bytes
 */
static char *
build_assertion(fe_skauth_state *state, int *outputlen)
{
	pg_sk_sign_params params;
	pg_sk_signature sig;
	int			result;
	char	   *output;
	uint8_t	   *p;
#ifndef WIN32
	fe_skauth_resident_key *current_key;
#endif

	memset(&params, 0, sizeof(params));
	memset(&sig, 0, sizeof(sig));

#ifndef WIN32
	/* Get the current resident key (the one whose public key server accepted) */
	if (state->current_key_index >= state->num_resident_keys)
	{
		libpq_append_conn_error(state->conn,
								"no resident key available for signing");
		return NULL;
	}
	current_key = &state->resident_keys[state->current_key_index];

	/* Set up signing parameters using the current key */
	params.application = SKAUTH_RP_ID;
	params.challenge = state->challenge;
	params.challenge_len = SKAUTH_CHALLENGE_LENGTH;
	params.device = state->conn->skauth_device;
	params.pin = state->conn->skauth_pin;
	params.key_handle = current_key->key_handle;
	params.key_handle_len = current_key->key_handle_len;
	params.flags = 0;

	if (state->options & SKAUTH_OPT_REQUIRE_UP)
		params.flags |= PG_SK_FLAG_REQUIRE_UP;
	if (state->options & SKAUTH_OPT_REQUIRE_UV)
		params.flags |= PG_SK_FLAG_REQUIRE_UV;

	SKAUTH_DEBUG("signing with application=\"%s\", key_handle_len=%zu",
				 params.application, params.key_handle_len);
	result = call_provider_sign(state, &params, &sig);

	if (result != PG_SK_ERR_SUCCESS)
	{
		libpq_append_conn_error(state->conn,
								"ssh-sk signing failed: %s",
								get_sign_error_string(state, result));
		return NULL;
	}

	/*
	 * Build response: sig_flags, counter, signature
	 * Total: 1 + 4 + 64 = 69 bytes
	 */
	output = malloc(1 + 4 + SKAUTH_ES256_SIG_LENGTH);
	if (!output)
	{
		free_provider_signature(state, &sig);
		return NULL;
	}

	p = (uint8_t *) output;

	/* sig_flags (1 byte) */
	*p++ = sig.flags;

	/* counter (4 bytes, big-endian) */
	*p++ = (sig.counter >> 24) & 0xFF;
	*p++ = (sig.counter >> 16) & 0xFF;
	*p++ = (sig.counter >> 8) & 0xFF;
	*p++ = sig.counter & 0xFF;

	/* signature (64 bytes) */
	memcpy(p, sig.signature, SKAUTH_ES256_SIG_LENGTH);

	free_provider_signature(state, &sig);

	SKAUTH_DEBUG("built assertion: flags=0x%02x, counter=%u", sig.flags, sig.counter);

	*outputlen = 1 + 4 + SKAUTH_ES256_SIG_LENGTH;
	return output;
#else
	libpq_append_conn_error(state->conn,
							"ssh-sk authentication not supported on this platform");
	return NULL;
#endif
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
	provider->sign = (pg_sk_sign_fn) dlsym(handle, "pg_sk_sign");
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
