/*-------------------------------------------------------------------------
 * auth-fido2.c
 *	  Server-side FIDO2 SASL authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-fido2.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_role_pubkeys.h"
#include "commands/user.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "libpq/fido2.h"
#include "libpq/sasl.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/syscache.h"

#include <string.h>

#ifdef USE_OPENSSL

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

/*
 * Verify an ES256 (ECDSA P-256) signature.
 *
 * pubkey: 65-byte uncompressed public key (0x04 || x || y)
 * hash: 32-byte SHA-256 hash of the signed data
 * sig: 64-byte raw signature (r || s, each 32 bytes)
 */
static Fido2VerifyResult
fido2_verify_es256_raw(const uint8_t *pubkey, const uint8_t *hash, const uint8_t *sig)
{
	EC_KEY	   *key = NULL;
	BIGNUM	   *x = NULL, *y = NULL, *r = NULL, *s = NULL;
	ECDSA_SIG  *esig = NULL;
	Fido2VerifyResult result = FIDO2_VERIFY_FAIL;

	if (pubkey[0] != 0x04)
		return FIDO2_VERIFY_FAIL;

	key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!key)
		goto done;

	x = BN_bin2bn(pubkey + 1, 32, NULL);
	y = BN_bin2bn(pubkey + 33, 32, NULL);
	if (!x || !y || EC_KEY_set_public_key_affine_coordinates(key, x, y) != 1)
		goto done;

	esig = ECDSA_SIG_new();
	r = BN_bin2bn(sig, 32, NULL);
	s = BN_bin2bn(sig + 32, 32, NULL);
	if (!esig || !r || !s || ECDSA_SIG_set0(esig, r, s) != 1)
		goto done;
	r = s = NULL;

	if (ECDSA_do_verify(hash, 32, esig, key) == 1)
		result = FIDO2_VERIFY_OK;

done:
	ECDSA_SIG_free(esig);
	BN_free(r);
	BN_free(s);
	BN_free(x);
	BN_free(y);
	EC_KEY_free(key);
	return result;
}

static void fido2_get_mechanisms(Port *port, StringInfo buf);
static void *fido2_init(Port *port, const char *selected_mech, const char *shadow_pass);
static int	fido2_exchange(void *opaq, const char *input, int inputlen,
							char **output, int *outputlen, const char **logdetail);

const pg_be_sasl_mech pg_be_fido2_mech = {
	fido2_get_mechanisms, fido2_init, fido2_exchange, FIDO2_MAX_ASSERTION_MSG
};

/* State machine states */
typedef enum {
	FIDO2_STATE_INIT = 0,
	FIDO2_STATE_CHALLENGE_SENT,
	FIDO2_STATE_FINISHED
} fido2_server_state;

typedef struct {
	fido2_server_state state;
	Port	   *port;
	Oid			roleid;
	uint8		challenge[FIDO2_CHALLENGE_LENGTH];
	Oid			cred_oid;
	char	   *key_name;
	int16		algorithm;
	uint8	   *public_key;
	int			public_key_len;
	bool		require_uv;
	bool		doomed;			/* true if auth will fail (user/key not found) */
	char	   *logdetail;
} fido2_state;

static void
fido2_get_mechanisms(Port *port, StringInfo buf)
{
	appendStringInfoString(buf, FIDO2_MECHANISM_NAME);
	appendStringInfoChar(buf, '\0');
}

static void *
fido2_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	fido2_state *st = (fido2_state *) palloc0(sizeof(fido2_state));
	st->port = port;
	st->roleid = get_role_oid(port->user_name, true);
	if (!OidIsValid(st->roleid))
	{
		st->doomed = true;
		st->logdetail = psprintf("Role \"%s\" does not exist", port->user_name);
		/* Create dummy data for constant-time verification */
		st->algorithm = COSE_ALG_ES256;
		st->public_key_len = FIDO2_ES256_PUBKEY_LENGTH;
		st->public_key = palloc(st->public_key_len);
		memset(st->public_key, 0, st->public_key_len);
		st->public_key[0] = 0x04;  /* Uncompressed point marker */
	}
	return st;
}

static int
fido2_exchange(void *opaq, const char *input, int inputlen,
				char **output, int *outputlen, const char **logdetail)
{
	fido2_state *st = (fido2_state *) opaq;

	*output = NULL;
	*outputlen = 0;
	*logdetail = NULL;

	if (st->state == FIDO2_STATE_INIT)
	{
		CatCList   *memlist;
		int			i;

		elog(DEBUG1, "FIDO2: received client-first-message (%d bytes, public key)",
			 inputlen);

		if (inputlen != FIDO2_ES256_PUBKEY_LENGTH)
		{
			*logdetail = "invalid public key length";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		/*
		 * Look up the public key. If the user doesn't exist (doomed) or the
		 * key isn't found, we continue the exchange anyway to prevent timing
		 * attacks that could enumerate valid usernames or credentials.
		 */
		if (!st->doomed)
		{
			memlist = SearchSysCacheList1(ROLEPUBKEYSROLEID, ObjectIdGetDatum(st->roleid));
			for (i = 0; i < memlist->n_members; i++)
			{
				HeapTuple	tuple = &memlist->members[i]->tuple;
				Form_pg_role_pubkeys pk = (Form_pg_role_pubkeys) GETSTRUCT(tuple);
				Datum		d;
				bool		isnull;
				bytea	   *stored;

				d = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple, Anum_pg_role_pubkeys_public_key, &isnull);
				if (isnull)
					continue;
				stored = DatumGetByteaP(d);
				if (VARSIZE_ANY_EXHDR(stored) == inputlen &&
					memcmp(VARDATA_ANY(stored), input, inputlen) == 0)
				{
					st->cred_oid = pk->oid;
					st->key_name = pstrdup(NameStr(pk->key_name));
					st->algorithm = pk->algorithm;
					st->public_key_len = inputlen;
					st->public_key = palloc(inputlen);
					memcpy(st->public_key, input, inputlen);
					break;
				}
			}
			ReleaseSysCacheList(memlist);

			if (!st->public_key)
			{
				st->doomed = true;
				st->logdetail = psprintf("public key not registered for role \"%s\"",
										 st->port->user_name);
				/* Create dummy data for constant-time verification */
				st->algorithm = COSE_ALG_ES256;
				st->public_key_len = FIDO2_ES256_PUBKEY_LENGTH;
				st->public_key = palloc(st->public_key_len);
				memset(st->public_key, 0, st->public_key_len);
				st->public_key[0] = 0x04;  /* Uncompressed point marker */
			}
		}

		if (!pg_strong_random(st->challenge, FIDO2_CHALLENGE_LENGTH))
			elog(ERROR, "could not generate random challenge");

		/* Build challenge: version(1) + challenge(32) + options(1) = 34 bytes */
		{
			StringInfoData buf;
			uint8 opts = FIDO2_OPT_REQUIRE_UP;
			if (st->require_uv)
				opts |= FIDO2_OPT_REQUIRE_UV;
			initStringInfo(&buf);
			appendStringInfoChar(&buf, FIDO2_PROTOCOL_VERSION);
			appendBinaryStringInfo(&buf, (char *) st->challenge, FIDO2_CHALLENGE_LENGTH);
			appendStringInfoChar(&buf, opts);
			*output = buf.data;
			*outputlen = buf.len;

			elog(DEBUG1, "FIDO2: sending server-challenge (version=%d, challenge=%d bytes, options=0x%02x)",
				 FIDO2_PROTOCOL_VERSION, FIDO2_CHALLENGE_LENGTH, opts);
		}
		st->state = FIDO2_STATE_CHALLENGE_SENT;
		return PG_SASL_EXCHANGE_CONTINUE;
	}

	if (st->state == FIDO2_STATE_CHALLENGE_SENT)
	{
		/* Verify assertion */
		const uint8 *p = (const uint8 *) input;
		uint8		sig_flags;
		uint32		counter;
		const uint8 *signature;
		uint8		rp_hash[PG_SHA256_DIGEST_LENGTH];
		uint8		auth_data[FIDO2_AUTH_DATA_LENGTH];
		uint8		client_hash[PG_SHA256_DIGEST_LENGTH];
		uint8		signed_hash[PG_SHA256_DIGEST_LENGTH];
		pg_cryptohash_ctx *ctx;

		if (inputlen != FIDO2_ASSERTION_LENGTH)
		{
			*logdetail = "invalid assertion length";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		sig_flags = p[0];

		/*
		 * Parse the signature counter. We intentionally do NOT validate that
		 * the counter has increased since the last authentication.
		 *
		 * While the FIDO2/WebAuthn spec recommends counter validation to detect
		 * cloned authenticators, many modern authenticators (especially those
		 * implementing CTAP 2.1+) set the counter to zero or don't increment
		 * reliably. Enforcing counter validation would break compatibility with
		 * common hardware security keys.
		 *
		 * The counter is logged at DEBUG1 level for informational purposes.
		 */
		counter = ((uint32) p[1] << 24) | ((uint32) p[2] << 16) |
				  ((uint32) p[3] << 8) | p[4];
		signature = p + 5;

		elog(DEBUG1, "FIDO2: received client-assertion (flags=0x%02x, counter=%u)",
			 sig_flags, counter);

		/*
		 * For doomed sessions (user doesn't exist or key not found), we still
		 * perform the hash computations to maintain consistent timing, then
		 * fail with the saved error message.
		 */

		/* Only check algorithm for non-doomed sessions */
		if (!st->doomed && st->algorithm != COSE_ALG_ES256)
		{
			*logdetail = "unsupported algorithm";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		if (!(sig_flags & FIDO2_FLAG_UP))
		{
			*logdetail = "user presence not verified";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		if (st->require_uv && !(sig_flags & FIDO2_FLAG_UV))
		{
			*logdetail = "user verification required";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		/* Compute rpIdHash */
		elog(DEBUG1, "FIDO2: computing rpIdHash for rpId=\"%s\"", FIDO2_RP_ID);
		ctx = pg_cryptohash_create(PG_SHA256);
		if (!ctx || pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, (uint8 *) FIDO2_RP_ID, strlen(FIDO2_RP_ID)) < 0 ||
			pg_cryptohash_final(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(ctx);
			*logdetail = "hash computation failed";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		pg_cryptohash_free(ctx);

		/* Build authenticatorData */
		elog(DEBUG1, "FIDO2: constructing authenticatorData (37 bytes)");
		memcpy(auth_data, rp_hash, 32);
		auth_data[32] = sig_flags;
		auth_data[33] = (counter >> 24) & 0xFF;
		auth_data[34] = (counter >> 16) & 0xFF;
		auth_data[35] = (counter >> 8) & 0xFF;
		auth_data[36] = counter & 0xFF;

		/*
		 * Compute clientDataHash = SHA256(challenge || rpIdHash)
		 *
		 * This follows the OpenSSH sk-provider convention where raw binary data
		 * (challenge || rpIdHash) is passed to sk_sign(), and the sk-provider
		 * internally computes SHA256 to produce the clientDataHash used in the
		 * FIDO2 assertion. The fido_assert_set_clientdata() function in libfido2
		 * performs this hashing automatically.
		 *
		 * This differs from WebAuthn, which uses SHA256(clientDataJSON) with a
		 * browser-provided JSON structure. The sk-provider API is designed for
		 * non-browser use cases like SSH and PostgreSQL authentication.
		 *
		 * Reference: OpenSSH PROTOCOL.u2f, lines 179-191
		 */
		ctx = pg_cryptohash_create(PG_SHA256);
		if (!ctx || pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, st->challenge, FIDO2_CHALLENGE_LENGTH) < 0 ||
			pg_cryptohash_update(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
			pg_cryptohash_final(ctx, client_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(ctx);
			*logdetail = "hash computation failed";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		pg_cryptohash_free(ctx);
		elog(DEBUG1, "FIDO2: clientDataHash computed");

		/* Compute signedDataHash = SHA256(authenticatorData || clientDataHash) */
		ctx = pg_cryptohash_create(PG_SHA256);
		if (!ctx || pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, auth_data, FIDO2_AUTH_DATA_LENGTH) < 0 ||
			pg_cryptohash_update(ctx, client_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
			pg_cryptohash_final(ctx, signed_hash, PG_SHA256_DIGEST_LENGTH) < 0)
		{
			pg_cryptohash_free(ctx);
			*logdetail = "hash computation failed";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		pg_cryptohash_free(ctx);

		/*
		 * Always perform verification for constant timing, even for doomed
		 * sessions. This prevents timing attacks that could enumerate valid
		 * usernames or credentials.
		 */
		{
			Fido2VerifyResult verify_result;

			verify_result = fido2_verify_es256_raw(st->public_key, signed_hash, signature);

			/* Check doomed AFTER verification to maintain timing */
			if (st->doomed || verify_result != FIDO2_VERIFY_OK)
			{
				elog(DEBUG1, "FIDO2: authentication failed (doomed=%d, verify=%d)",
					 st->doomed, verify_result);
				*logdetail = st->doomed ? st->logdetail : "signature verification failed";
				return PG_SASL_EXCHANGE_FAILURE;
			}
		}

		elog(DEBUG1, "FIDO2: signature verification succeeded");
		st->state = FIDO2_STATE_FINISHED;
		return PG_SASL_EXCHANGE_SUCCESS;
	}

	return PG_SASL_EXCHANGE_FAILURE;
}

#endif							/* USE_OPENSSL */
