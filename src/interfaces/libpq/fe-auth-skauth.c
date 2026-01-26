/*-------------------------------------------------------------------------
 * fe-auth-skauth.c
 *	  Client-side skauth SASL authentication using OpenSSH sk-api
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-skauth.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/skauth.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

#include <string.h>

#define PGSSHSKKEY_ENV "PGSSHSKKEY"

#ifndef WIN32

/* OpenSSH sk-api definitions */
#define SSH_SK_VERSION_MAJOR		0x000a0000
#define SSH_SK_VERSION_MAJOR_MASK	0xffff0000
#define SSH_SK_ECDSA				0x00
#define SSH_SK_USER_PRESENCE_REQD	0x01
#define SSH_SK_USER_VERIFICATION_REQD	0x04

struct sk_sign_response {
	uint8_t		flags;
	uint32_t	counter;
	uint8_t	   *sig_r;
	size_t		sig_r_len;
	uint8_t	   *sig_s;
	size_t		sig_s_len;
};

struct sk_option {
	char	   *name;
	char	   *value;
	uint8_t		required;
};

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

typedef uint32_t (*sk_api_version_fn)(void);
typedef int (*sk_sign_fn)(uint32_t alg, const uint8_t *data, size_t data_len,
						  const char *application,
						  const uint8_t *key_handle, size_t key_handle_len,
						  uint8_t flags, const char *pin,
						  struct sk_option **options,
						  struct sk_sign_response **sign_response);
typedef int (*sk_load_resident_keys_fn)(const char *pin,
										struct sk_option **options,
										struct sk_resident_key ***rks,
										size_t *nrks);

typedef struct {
	uint8_t	   *public_key;
	size_t		public_key_len;
	uint8_t	   *key_handle;
	size_t		key_handle_len;
} fe_skauth_key;

typedef struct {
	PGconn	   *conn;
	void	   *handle;
	sk_sign_fn	sign;
	sk_load_resident_keys_fn load_keys;
	fe_skauth_key *keys;
	size_t		num_keys;
	size_t		key_idx;
	uint8_t		challenge[SKAUTH_CHALLENGE_LENGTH];
	uint8_t		options;
	int			state;
} fe_skauth_state;

static void *skauth_init(PGconn *conn, const char *password, const char *mech);
static SASLStatus skauth_exchange(void *state, bool final,
								  char *input, int inputlen,
								  char **output, int *outputlen);
static bool skauth_channel_bound(void *state);
static void skauth_free(void *state);

const pg_fe_sasl_mech pg_skauth_mech = {
	skauth_init, skauth_exchange, skauth_channel_bound, skauth_free
};

static void *
skauth_init(PGconn *conn, const char *password, const char *mech)
{
	fe_skauth_state *st;
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

	st = malloc(sizeof(fe_skauth_state));
	if (!st)
		return NULL;
	memset(st, 0, sizeof(fe_skauth_state));
	st->conn = conn;

	st->handle = dlopen(path, RTLD_NOW);
	if (!st->handle)
	{
		libpq_append_conn_error(conn, "failed to load sk-provider: %s", dlerror());
		free(st);
		return NULL;
	}

	version_fn = (sk_api_version_fn) dlsym(st->handle, "sk_api_version");
	st->sign = (sk_sign_fn) dlsym(st->handle, "sk_sign");
	st->load_keys = (sk_load_resident_keys_fn) dlsym(st->handle, "sk_load_resident_keys");

	if (!version_fn || !st->sign || !st->load_keys ||
		(version_fn() & SSH_SK_VERSION_MAJOR_MASK) != SSH_SK_VERSION_MAJOR)
	{
		libpq_append_conn_error(conn, "invalid sk-provider");
		dlclose(st->handle);
		free(st);
		return NULL;
	}

	if (st->load_keys(conn->skauth_pin, NULL, &rks, &nrks) != 0 || !rks)
	{
		libpq_append_conn_error(conn, "failed to load resident keys");
		dlclose(st->handle);
		free(st);
		return NULL;
	}

	/* Count and copy matching keys in single pass */
	for (i = 0; i < nrks; i++)
		if (rks[i]->application && strcmp(rks[i]->application, SKAUTH_RP_ID) == 0)
			st->num_keys++;

	if (st->num_keys == 0)
	{
		libpq_append_conn_error(conn, "no ssh: credentials on security key");
		goto free_rks;
	}

	st->keys = malloc(sizeof(fe_skauth_key) * st->num_keys);
	if (!st->keys)
		goto free_rks;

	for (i = 0, n = 0; i < nrks; i++)
	{
		if (rks[i]->application && strcmp(rks[i]->application, SKAUTH_RP_ID) == 0)
		{
			fe_skauth_key *k = &st->keys[n++];
			k->public_key_len = rks[i]->key.public_key_len;
			k->public_key = malloc(k->public_key_len);
			k->key_handle_len = rks[i]->key.key_handle_len;
			k->key_handle = malloc(k->key_handle_len);
			if (k->public_key && k->key_handle)
			{
				memcpy(k->public_key, rks[i]->key.public_key, k->public_key_len);
				memcpy(k->key_handle, rks[i]->key.key_handle, k->key_handle_len);
			}
		}
		free(rks[i]->application);
		free(rks[i]->key.public_key);
		free(rks[i]->key.key_handle);
		free(rks[i]->user_id);
		free(rks[i]);
	}
	free(rks);

	/* Handle PGSSHSKKEY selection */
	key_str = getenv(PGSSHSKKEY_ENV);
	if (key_str && key_str[0])
	{
		SkauthParsedPubkey parsed;
		char *err = NULL;
		if (skauth_parse_openssh_pubkey(key_str, &parsed, &err))
		{
			for (i = 0; i < st->num_keys; i++)
			{
				if (st->keys[i].public_key_len == parsed.public_key_len &&
					memcmp(st->keys[i].public_key, parsed.public_key, parsed.public_key_len) == 0)
				{
					st->key_idx = i;
					break;
				}
			}
			skauth_free_parsed_pubkey(&parsed);
		}
		free(err);
	}

	return st;

free_rks:
	for (i = 0; i < nrks; i++)
	{
		free(rks[i]->application);
		free(rks[i]->key.public_key);
		free(rks[i]->key.key_handle);
		free(rks[i]->user_id);
		free(rks[i]);
	}
	free(rks);
	dlclose(st->handle);
	free(st);
	return NULL;
}

static SASLStatus
skauth_exchange(void *opaque, bool final, char *input, int inputlen,
				char **output, int *outputlen)
{
	fe_skauth_state *st = (fe_skauth_state *) opaque;
	fe_skauth_key *key;
	const uint8_t *p;
	struct sk_sign_response *sig = NULL;
	uint8_t		ext[SKAUTH_CHALLENGE_LENGTH + PG_SHA256_DIGEST_LENGTH];
	uint8_t		rp_hash[PG_SHA256_DIGEST_LENGTH];
	pg_cryptohash_ctx *ctx;
	uint8_t		flags;
	uint8_t	   *out;

	*output = NULL;
	*outputlen = 0;

	if (st->state == 0)
	{
		/* Send public key */
		key = &st->keys[st->key_idx];
		*output = malloc(key->public_key_len);
		if (!*output)
			return SASL_FAILED;
		memcpy(*output, key->public_key, key->public_key_len);
		*outputlen = key->public_key_len;
		st->state = 1;
		return SASL_CONTINUE;
	}

	if (st->state == 1)
	{
		/* Parse challenge */
		p = (const uint8_t *) input;
		if (inputlen < 34 || p[0] != SKAUTH_PROTOCOL_VERSION)
		{
			libpq_append_conn_error(st->conn, "invalid challenge");
			return SASL_FAILED;
		}
		memcpy(st->challenge, p + 1, SKAUTH_CHALLENGE_LENGTH);
		st->options = p[33];

		/* Compute extended challenge */
		ctx = pg_cryptohash_create(PG_SHA256);
		if (!ctx ||
			pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, (const uint8_t *) SKAUTH_RP_ID, 4) < 0 ||
			pg_cryptohash_final(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(ctx);
			return SASL_FAILED;
		}
		pg_cryptohash_free(ctx);

		memcpy(ext, st->challenge, SKAUTH_CHALLENGE_LENGTH);
		memcpy(ext + SKAUTH_CHALLENGE_LENGTH, rp_hash, PG_SHA256_DIGEST_LENGTH);

		/* Sign */
		key = &st->keys[st->key_idx];
		flags = 0;
		if (st->options & SKAUTH_OPT_REQUIRE_UP)
			flags |= SSH_SK_USER_PRESENCE_REQD;
		if (st->options & SKAUTH_OPT_REQUIRE_UV)
			flags |= SSH_SK_USER_VERIFICATION_REQD;

		if (st->sign(SSH_SK_ECDSA, ext, sizeof(ext), SKAUTH_RP_ID,
					 key->key_handle, key->key_handle_len, flags,
					 st->conn->skauth_pin, NULL, &sig) != 0 || !sig)
		{
			libpq_append_conn_error(st->conn, "signing failed");
			return SASL_FAILED;
		}

		/* Build response: flags(1) + counter(4) + signature(64) */
		out = malloc(69);
		if (!out)
		{
			free(sig->sig_r);
			free(sig->sig_s);
			free(sig);
			return SASL_FAILED;
		}
		out[0] = sig->flags;
		out[1] = (sig->counter >> 24) & 0xFF;
		out[2] = (sig->counter >> 16) & 0xFF;
		out[3] = (sig->counter >> 8) & 0xFF;
		out[4] = sig->counter & 0xFF;
		memcpy(out + 5, sig->sig_r, sig->sig_r_len);
		memcpy(out + 5 + sig->sig_r_len, sig->sig_s, sig->sig_s_len);

		free(sig->sig_r);
		free(sig->sig_s);
		free(sig);

		*output = (char *) out;
		*outputlen = 69;
		st->state = 2;
		return final ? SASL_COMPLETE : SASL_CONTINUE;
	}

	return final ? SASL_COMPLETE : SASL_FAILED;
}

static bool
skauth_channel_bound(void *state)
{
	return false;
}

static void
skauth_free(void *opaque)
{
	fe_skauth_state *st = (fe_skauth_state *) opaque;
	size_t i;

	if (!st)
		return;

	if (st->state != 2 && st->num_keys > 1)
		fprintf(stderr, "HINT: Set PGSSHSKKEY to select a different key.\n");

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

const pg_fe_sasl_mech pg_skauth_mech = {NULL, NULL, NULL, NULL};

#endif
