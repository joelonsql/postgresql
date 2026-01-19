/*-------------------------------------------------------------------------
 *
 * auth-skauth.c
 *	  Server-side implementation of ssh-sk SASL authentication.
 *
 * This implements a SASL mechanism named "ssh-sk" for authenticating
 * users with hardware security keys (e.g., YubiKey) or platform authenticators
 * (e.g., macOS Secure Enclave via Touch ID).
 *
 * The authentication flow follows the SSH model:
 * 1. Client sends a public key (from a resident credential on the authenticator)
 * 2. Server looks up the key; if registered, sends a challenge
 * 3. Client signs the challenge using the security key
 * 4. Server verifies the signature using OpenSSL's ECDSA implementation
 *
 * If the server doesn't recognize the key, it returns an error and the
 * client can restart SASL with the next available key.
 *
 * See src/include/libpq/skauth.h for protocol details.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-skauth.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_role_pubkeys.h"
#include "commands/user.h"
#include "common/cryptohash.h"
#include "common/skauth-verify.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "libpq/skauth.h"
#include "libpq/sasl.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/catcache.h"
#include "utils/syscache.h"

#include <string.h>

#ifdef USE_OPENSSL

/* Forward declarations */
static void skauth_get_mechanisms(Port *port, StringInfo buf);
static void *skauth_init(Port *port, const char *selected_mech,
						const char *shadow_pass);
static int	skauth_exchange(void *opaq, const char *input, int inputlen,
						   char **output, int *outputlen,
						   const char **logdetail);

/* Mechanism declaration */
const pg_be_sasl_mech pg_be_skauth_mech = {
	skauth_get_mechanisms,
	skauth_init,
	skauth_exchange,
	SKAUTH_MAX_ASSERTION_MSG
};

/*
 * State for a ssh-sk authentication exchange
 */
typedef enum
{
	SKAUTH_AUTH_INIT,
	SKAUTH_AUTH_CHALLENGE_SENT,
	SKAUTH_AUTH_FINISHED
} skauth_state_enum;

/*
 * Credential info from pg_role_pubkeys
 */
typedef struct SkauthCredential
{
	Oid			oid;
	char	   *key_name;
	int16		algorithm;
	uint8	   *public_key;
	int			public_key_len;
} SkauthCredential;

typedef struct
{
	skauth_state_enum state;

	Port	   *port;
	Oid			roleid;

	/* Challenge sent to client */
	uint8		challenge[SKAUTH_CHALLENGE_LENGTH];

	/* Matched credential (found when client sends public key) */
	SkauthCredential *matched_cred;

	/* Options from HBA */
	bool		require_uv;

	/* If doomed, we continue mock authentication */
	bool		doomed;
	char	   *logdetail;
} skauth_state;

/* Helper functions */
static SkauthCredential *lookup_public_key(skauth_state *state,
										   const uint8 *public_key, int public_key_len);
static char *build_challenge_message(skauth_state *state, int *len);
static bool verify_assertion(skauth_state *state, const char *input, int inputlen,
							 const char **logdetail);

/*
 * Get list of SASL mechanisms supported
 */
static void
skauth_get_mechanisms(Port *port, StringInfo buf)
{
	appendStringInfoString(buf, SKAUTH_MECHANISM_NAME);
	appendStringInfoChar(buf, '\0');
}

/*
 * Initialize ssh-sk authentication state
 */
static void *
skauth_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	skauth_state *state;

	state = (skauth_state *) palloc0(sizeof(skauth_state));
	state->state = SKAUTH_AUTH_INIT;
	state->port = port;
	state->doomed = false;
	state->matched_cred = NULL;

	/* Look up the user */
	state->roleid = get_role_oid(port->user_name, true);
	elog(DEBUG1, "skauth: authenticating user \"%s\"", port->user_name);
	if (!OidIsValid(state->roleid))
	{
		state->doomed = true;
		state->logdetail = psprintf("Role \"%s\" does not exist",
									port->user_name);
	}

	/* Get options from HBA line */
	state->require_uv = false;	/* TODO: get from port->hba */

	return state;
}

/*
 * Exchange ssh-sk messages
 */
static int
skauth_exchange(void *opaq, const char *input, int inputlen,
			   char **output, int *outputlen,
			   const char **logdetail)
{
	skauth_state *state = (skauth_state *) opaq;

	*output = NULL;
	*outputlen = 0;
	*logdetail = NULL;

	elog(DEBUG1, "skauth: exchange state=%d, inputlen=%d", state->state, inputlen);

	switch (state->state)
	{
		case SKAUTH_AUTH_INIT:
			/*
			 * First message from client contains the public key (65 bytes).
			 * Look it up and send challenge if registered.
			 */
			if (state->doomed)
			{
				/* Role doesn't exist - fail now */
				elog(DEBUG1, "skauth: authentication failed (doomed): %s",
					 state->logdetail ? state->logdetail : "unknown error");
				*logdetail = state->logdetail;
				return PG_SASL_EXCHANGE_FAILURE;
			}

			if (inputlen != SKAUTH_ES256_PUBKEY_LENGTH)
			{
				elog(DEBUG1, "skauth: invalid public key length %d (expected %d)",
					 inputlen, SKAUTH_ES256_PUBKEY_LENGTH);
				*logdetail = "invalid public key length";
				return PG_SASL_EXCHANGE_FAILURE;
			}

			/* Look up the public key */
			state->matched_cred = lookup_public_key(state,
													(const uint8 *) input,
													inputlen);
			if (state->matched_cred == NULL)
			{
				/*
				 * Public key not registered for this user.
				 * Log the first few bytes of the key to help with debugging.
				 */
				elog(DEBUG1, "skauth: public key not registered for user \"%s\" "
					 "(key starts with %02x%02x%02x%02x...)",
					 state->port->user_name,
					 (uint8) input[0], (uint8) input[1],
					 (uint8) input[2], (uint8) input[3]);

				/*
				 * Provide a helpful error message. The hint about PGSSHSKKEY
				 * is shown on the client side (only if there are multiple keys).
				 */
				*logdetail = psprintf("public key not registered for role \"%s\". "
									  "Run 'ssh-add -L' to see available keys.",
									  state->port->user_name);
				return PG_SASL_EXCHANGE_FAILURE;
			}

			elog(DEBUG1, "skauth: matched credential \"%s\" for user \"%s\"",
				 state->matched_cred->key_name, state->port->user_name);

			/* Generate challenge */
			if (!pg_strong_random(state->challenge, SKAUTH_CHALLENGE_LENGTH))
				elog(ERROR, "could not generate random ssh-sk challenge");

			/* Send challenge to client */
			*output = build_challenge_message(state, outputlen);
			elog(DEBUG1, "skauth: sending challenge message (%d bytes)",
				 *outputlen);
			state->state = SKAUTH_AUTH_CHALLENGE_SENT;
			return PG_SASL_EXCHANGE_CONTINUE;

		case SKAUTH_AUTH_CHALLENGE_SENT:
			/* Client sent assertion response */
			elog(DEBUG1, "skauth: received assertion (%d bytes), verifying", inputlen);
			if (!verify_assertion(state, input, inputlen, logdetail))
			{
				elog(DEBUG1, "skauth: authentication failed: %s",
					 *logdetail ? *logdetail : "unknown error");
				return PG_SASL_EXCHANGE_FAILURE;
			}

			state->state = SKAUTH_AUTH_FINISHED;
			return PG_SASL_EXCHANGE_SUCCESS;

		case SKAUTH_AUTH_FINISHED:
			elog(ERROR, "skauth exchange already finished");
			break;
	}

	/* Should not reach here */
	return PG_SASL_EXCHANGE_FAILURE;
}

/*
 * Look up a public key in pg_role_pubkeys for the current user.
 *
 * Returns a palloc'd SkauthCredential if found, NULL otherwise.
 *
 * Uses syscache instead of table scans because this runs during authentication,
 * before a database has been selected. Syscache has special handling for
 * pre-database-selected access, while table_open() would fail with
 * "cannot read pg_class without having selected a database".
 */
static SkauthCredential *
lookup_public_key(skauth_state *state, const uint8 *public_key, int public_key_len)
{
	CatCList   *memlist;
	SkauthCredential *result = NULL;
	int			i;

	/* Use syscache to get all credentials for this role */
	elog(DEBUG1, "skauth: searching pg_role_pubkeys for role OID %u", state->roleid);
	memlist = SearchSysCacheList1(ROLEPUBKEYSROLEID,
								  ObjectIdGetDatum(state->roleid));

	for (i = 0; i < memlist->n_members; i++)
	{
		HeapTuple	tuple = &memlist->members[i]->tuple;
		Form_pg_role_pubkeys pubkey = (Form_pg_role_pubkeys) GETSTRUCT(tuple);
		Datum		datum;
		bool		isnull;
		bytea	   *stored_pubkey;

		/* Get public_key (bytea) */
		datum = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple,
								Anum_pg_role_pubkeys_public_key, &isnull);
		if (isnull)
			continue;

		stored_pubkey = DatumGetByteaP(datum);

		/* Check if this matches the client's public key */
		if (VARSIZE_ANY_EXHDR(stored_pubkey) == public_key_len &&
			memcmp(VARDATA_ANY(stored_pubkey), public_key, public_key_len) == 0)
		{
			/* Found a match */
			result = (SkauthCredential *) palloc0(sizeof(SkauthCredential));
			result->oid = pubkey->oid;
			result->key_name = pstrdup(NameStr(pubkey->key_name));
			result->algorithm = pubkey->algorithm;
			result->public_key_len = public_key_len;
			result->public_key = palloc(public_key_len);
			memcpy(result->public_key, public_key, public_key_len);

			elog(DEBUG1, "skauth: found matching credential \"%s\" (algorithm=%d)",
				 result->key_name, result->algorithm);
			break;
		}
	}

	ReleaseSysCacheList(memlist);
	return result;
}

/*
 * Build the challenge message to send to the client
 *
 * Format:
 *   protocol_version: 1 byte
 *   challenge: 32 bytes
 *   options: 1 byte
 *
 * Total: 34 bytes
 */
static char *
build_challenge_message(skauth_state *state, int *len)
{
	StringInfoData buf;
	uint8		options;

	initStringInfo(&buf);

	/* Protocol version */
	appendStringInfoChar(&buf, SKAUTH_PROTOCOL_VERSION);

	/* Challenge */
	appendBinaryStringInfo(&buf, (const char *) state->challenge,
						   SKAUTH_CHALLENGE_LENGTH);

	/* Options */
	options = SKAUTH_OPT_REQUIRE_UP;
	if (state->require_uv)
		options |= SKAUTH_OPT_REQUIRE_UV;
	appendStringInfoChar(&buf, options);

	elog(DEBUG1, "skauth: building challenge message: options=0x%02x", options);

	*len = buf.len;
	return buf.data;
}

/*
 * Parse and verify the assertion from the client
 *
 * Simplified assertion format:
 *   sig_flags: 1 byte (from authenticator)
 *   signature: 64 bytes (R || S)
 *
 * Total: 65 bytes
 *
 * Note: We don't use the signature counter for replay protection because
 * modern FIDO2 authenticators often return 0 for privacy reasons. The
 * counter is still included in the signed authenticator data (from the
 * hardware) but not transmitted separately in our protocol.
 */
static bool
verify_assertion(skauth_state *state, const char *input, int inputlen,
				 const char **logdetail)
{
	const uint8 *p = (const uint8 *) input;
	const uint8 *end = p + inputlen;
	uint8		sig_flags;
	uint32		counter;
	const uint8 *signature;
	SkauthCredential *matched_cred = state->matched_cred;
	uint8		rp_id_hash[PG_SHA256_DIGEST_LENGTH];
	uint8		auth_data[37];		/* rpIdHash(32) + flags(1) + counter(4) */
	uint8		client_data_hash[PG_SHA256_DIGEST_LENGTH];
	uint8		signed_data_hash[PG_SHA256_DIGEST_LENGTH];
	pg_cryptohash_ctx *sha256_ctx;
	SkauthVerifyResult verify_result;

	/* Should have matched credential from client-first message */
	if (matched_cred == NULL)
	{
		*logdetail = "no credential matched (internal error)";
		return false;
	}

	/* Parse sig_flags (1 byte) */
	if (end - p < 1)
	{
		*logdetail = "assertion too short: missing sig_flags";
		return false;
	}
	sig_flags = *p++;
	elog(DEBUG1, "skauth: sig_flags=0x%02x", sig_flags);

	/* Parse counter (4 bytes, big-endian) - used only for signature verification */
	if (end - p < 4)
	{
		*logdetail = "assertion too short: missing counter";
		return false;
	}
	counter = ((uint32) p[0] << 24) | ((uint32) p[1] << 16) |
			  ((uint32) p[2] << 8) | (uint32) p[3];
	p += 4;
	elog(DEBUG1, "skauth: counter=%u (not validated)", counter);

	/* Parse signature (64 bytes) */
	if (end - p < SKAUTH_ES256_SIG_LENGTH)
	{
		*logdetail = "assertion too short: signature truncated";
		return false;
	}
	signature = p;
	elog(DEBUG1, "skauth: parsed signature (%d bytes)", SKAUTH_ES256_SIG_LENGTH);

	/* Only ES256 is supported */
	if (matched_cred->algorithm != COSE_ALG_ES256)
	{
		*logdetail = "unsupported credential algorithm";
		return false;
	}

	/* Check user present flag */
	elog(DEBUG1, "skauth: user_present=%d, user_verified=%d",
		 (sig_flags & SKAUTH_FLAG_UP) != 0,
		 (sig_flags & SKAUTH_FLAG_UV) != 0);
	if (!(sig_flags & SKAUTH_FLAG_UP))
	{
		*logdetail = "user present flag not set";
		return false;
	}

	/* Check user verified flag if required */
	if (state->require_uv && !(sig_flags & SKAUTH_FLAG_UV))
	{
		*logdetail = "user verification required but not performed";
		return false;
	}

	/*
	 * Reconstruct authenticatorData for signature verification.
	 * Format: rpIdHash (32 bytes) || flags (1 byte) || counter (4 bytes)
	 *
	 * Use hardcoded RP ID "ssh:" for SSH security keys.
	 *
	 * Note: The counter is part of the signed authenticator data even though
	 * we don't validate it separately, since the authenticator includes it
	 * in what it signs.
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx ||
		pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, (const uint8 *) SKAUTH_RP_ID,
							 strlen(SKAUTH_RP_ID)) < 0 ||
		pg_cryptohash_final(sha256_ctx, rp_id_hash, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		if (sha256_ctx)
			pg_cryptohash_free(sha256_ctx);
		*logdetail = "SHA-256 computation failed";
		return false;
	}
	pg_cryptohash_free(sha256_ctx);

	/* Build authenticatorData: rpIdHash || flags || counter */
	memcpy(auth_data, rp_id_hash, 32);
	auth_data[32] = sig_flags;
	auth_data[33] = (counter >> 24) & 0xFF;
	auth_data[34] = (counter >> 16) & 0xFF;
	auth_data[35] = (counter >> 8) & 0xFF;
	auth_data[36] = counter & 0xFF;

	/*
	 * Compute client data hash: SHA256(challenge || rp_id_hash)
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx ||
		pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, state->challenge, SKAUTH_CHALLENGE_LENGTH) < 0 ||
		pg_cryptohash_update(sha256_ctx, rp_id_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
		pg_cryptohash_final(sha256_ctx, client_data_hash, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		if (sha256_ctx)
			pg_cryptohash_free(sha256_ctx);
		*logdetail = "SHA-256 computation failed";
		return false;
	}
	pg_cryptohash_free(sha256_ctx);

	/*
	 * Compute signed data hash: SHA256(authenticatorData || clientDataHash)
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx ||
		pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, auth_data, 37) < 0 ||
		pg_cryptohash_update(sha256_ctx, client_data_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
		pg_cryptohash_final(sha256_ctx, signed_data_hash, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		if (sha256_ctx)
			pg_cryptohash_free(sha256_ctx);
		*logdetail = "SHA-256 computation failed";
		return false;
	}
	pg_cryptohash_free(sha256_ctx);

	/*
	 * Verify the signature.
	 * The public key is stored as uncompressed EC point (65 bytes: 0x04 || X || Y)
	 */
	if (matched_cred->public_key_len != SKAUTH_ES256_PUBKEY_LENGTH)
	{
		*logdetail = "invalid public key length";
		return false;
	}

	elog(DEBUG1, "skauth: verifying ES256 signature");
	verify_result = skauth_verify_es256_raw(matched_cred->public_key,
											signed_data_hash,
											signature);

	if (verify_result != SKAUTH_VERIFY_OK)
	{
		*logdetail = "signature verification failed";
		return false;
	}

	elog(DEBUG1, "skauth: signature verification successful");

	return true;
}

#endif							/* USE_OPENSSL */
