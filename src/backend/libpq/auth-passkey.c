/*-------------------------------------------------------------------------
 * auth-passkey.c
 *	  Server-side Passkey SASL authentication
 *
 * This implements WebAuthn-style passkey authentication using native
 * platform credentials (as opposed to FIDO2 which uses OpenSSH sk-api).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-passkey.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_role_pubkeys.h"
#include "commands/user.h"
#include "common/base64.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "libpq/crypt.h"
#include "libpq/passkey.h"
#include "libpq/sasl.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/memutils.h"

#include <string.h>

#ifdef USE_OPENSSL

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

/* GUC variable for relying party ID */
char	   *passkey_relying_party_id = NULL;

/*
 * Pending credential to be stored after database init.
 * During authentication, we can't access catalogs yet because MyDatabaseId
 * isn't set. We store the credential here and write it after init completes.
 */
typedef struct PendingPasskeyCredential
{
	bool		pending;
	Oid			roleid;
	char	   *rp_id;
	uint8	   *credential_id;
	int			credential_id_len;
	uint8	   *public_key;
	int			public_key_len;
} PendingPasskeyCredential;

static PendingPasskeyCredential pending_cred = {0};

/*
 * Verify an ES256 (ECDSA P-256) signature with DER encoding.
 *
 * pubkey: 65-byte uncompressed public key (0x04 || x || y)
 * hash: 32-byte SHA-256 hash of the signed data
 * sig: DER-encoded ECDSA signature
 * siglen: length of signature
 */
static PasskeyVerifyResult
passkey_verify_es256(const uint8_t *pubkey, const uint8_t *hash,
					 const uint8_t *sig, size_t siglen)
{
	EC_KEY	   *key = NULL;
	BIGNUM	   *x = NULL, *y = NULL;
	const uint8_t *sigptr = sig;
	ECDSA_SIG  *esig = NULL;
	PasskeyVerifyResult result = PASSKEY_VERIFY_FAIL;

	if (pubkey[0] != 0x04)
		return PASSKEY_VERIFY_FAIL;

	key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!key)
		goto done;

	x = BN_bin2bn(pubkey + 1, 32, NULL);
	y = BN_bin2bn(pubkey + 33, 32, NULL);
	if (!x || !y || EC_KEY_set_public_key_affine_coordinates(key, x, y) != 1)
		goto done;

	/* Parse DER-encoded signature */
	esig = d2i_ECDSA_SIG(NULL, &sigptr, siglen);
	if (!esig)
		goto done;

	if (ECDSA_do_verify(hash, 32, esig, key) == 1)
		result = PASSKEY_VERIFY_OK;

done:
	ECDSA_SIG_free(esig);
	BN_free(x);
	BN_free(y);
	EC_KEY_free(key);
	return result;
}

static void passkey_get_mechanisms(Port *port, StringInfo buf);
static void *passkey_init(Port *port, const char *selected_mech, const char *shadow_pass);
static int	passkey_exchange(void *opaq, const char *input, int inputlen,
							 char **output, int *outputlen, const char **logdetail);

const pg_be_sasl_mech pg_be_passkey_mech = {
	passkey_get_mechanisms, passkey_init, passkey_exchange, PASSKEY_MAX_RESPONSE_MSG
};

/* State machine states */
typedef enum {
	PASSKEY_STATE_INIT = 0,
	PASSKEY_STATE_PASSWORD_REQUESTED,
	PASSKEY_STATE_PASSWORD_VERIFIED,
	PASSKEY_STATE_CHALLENGE_SENT,
	PASSKEY_STATE_FINISHED
} passkey_server_state;

typedef struct {
	passkey_server_state state;
	Port	   *port;
	Oid			roleid;
	uint8		challenge[PASSKEY_CHALLENGE_LENGTH];
	char	   *rp_id;				/* relying party ID */
	Oid			cred_oid;
	char	   *key_name;
	int16		algorithm;
	uint8	   *public_key;
	int			public_key_len;
	uint8	   *credential_id;
	int			credential_id_len;
	bool		require_uv;
	bool		doomed;				/* true if auth will fail */
	char	   *logdetail;
	char	   *shadow_pass;		/* stored password verifier */
	bool		is_registration;	/* true if this is MakeCredential */
	uint8	   *user_id;			/* user ID for registration */
	int			user_id_len;
} passkey_state;

static void
passkey_get_mechanisms(Port *port, StringInfo buf)
{
	appendStringInfoString(buf, PASSKEY_MECHANISM_NAME);
	appendStringInfoChar(buf, '\0');
}

/*
 * Get the effective relying party ID.
 * Priority: GUC > server hostname
 */
static char *
get_effective_rp_id(Port *port)
{
	if (passkey_relying_party_id && passkey_relying_party_id[0])
		return pstrdup(passkey_relying_party_id);
	if (port->remote_hostname && port->remote_hostname[0])
		return pstrdup(port->remote_hostname);
	return pstrdup("localhost");
}

static void *
passkey_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	passkey_state *st = (passkey_state *) palloc0(sizeof(passkey_state));

	st->port = port;
	st->rp_id = get_effective_rp_id(port);
	st->roleid = get_role_oid(port->user_name, true);

	if (!OidIsValid(st->roleid))
	{
		st->doomed = true;
		st->logdetail = psprintf("Role \"%s\" does not exist", port->user_name);
		/* Create dummy data for constant-time verification */
		st->algorithm = COSE_ALG_ES256;
		st->public_key_len = PASSKEY_ES256_PUBKEY_LENGTH;
		st->public_key = palloc(st->public_key_len);
		memset(st->public_key, 0, st->public_key_len);
		st->public_key[0] = 0x04;  /* Uncompressed point marker */
	}

	return st;
}

/*
 * Helper function to generate user_id from username.
 * Uses SHA-256 hash of username, truncated to 16 bytes.
 */
static void
generate_user_id(const char *username, uint8 *user_id, int *user_id_len)
{
	pg_cryptohash_ctx *ctx;
	uint8		hash[PG_SHA256_DIGEST_LENGTH];

	ctx = pg_cryptohash_create(PG_SHA256);
	if (ctx && pg_cryptohash_init(ctx) >= 0 &&
		pg_cryptohash_update(ctx, (const uint8 *) username, strlen(username)) >= 0 &&
		pg_cryptohash_final(ctx, hash, PG_SHA256_DIGEST_LENGTH) >= 0)
	{
		/* Use first 16 bytes of hash as user_id */
		memcpy(user_id, hash, 16);
		*user_id_len = 16;
	}
	pg_cryptohash_free(ctx);
}

/*
 * Helper function to store a new credential in pg_role_pubkeys.
 */
static bool
store_passkey_credential(Oid roleid, const char *rp_id,
						 const uint8 *credential_id, int credential_id_len,
						 const uint8 *public_key, int public_key_len)
{
	Relation	rel;
	HeapTuple	tuple;
	Datum		values[Natts_pg_role_pubkeys];
	bool		nulls[Natts_pg_role_pubkeys];
	Oid			new_oid;
	bytea	   *pk_bytea;
	bytea	   *cid_bytea;

	/* Build bytea values for credential_id and public_key */
	cid_bytea = palloc(VARHDRSZ + credential_id_len);
	SET_VARSIZE(cid_bytea, VARHDRSZ + credential_id_len);
	memcpy(VARDATA(cid_bytea), credential_id, credential_id_len);

	pk_bytea = palloc(VARHDRSZ + public_key_len);
	SET_VARSIZE(pk_bytea, VARHDRSZ + public_key_len);
	memcpy(VARDATA(pk_bytea), public_key, public_key_len);

	/* Open the pg_role_pubkeys catalog */
	rel = table_open(RolePubkeysRelationId, RowExclusiveLock);

	/* Initialize values array */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	new_oid = GetNewOidWithIndex(rel, RolePubkeysOidIndexId, Anum_pg_role_pubkeys_oid);

	values[Anum_pg_role_pubkeys_oid - 1] = ObjectIdGetDatum(new_oid);
	values[Anum_pg_role_pubkeys_roleid - 1] = ObjectIdGetDatum(roleid);
	values[Anum_pg_role_pubkeys_key_name - 1] = DirectFunctionCall1(namein, CStringGetDatum("passkey"));
	values[Anum_pg_role_pubkeys_algorithm - 1] = Int16GetDatum(COSE_ALG_ES256);
	values[Anum_pg_role_pubkeys_credential_type - 1] = Int16GetDatum(CRED_TYPE_WEBAUTHN);
	values[Anum_pg_role_pubkeys_public_key - 1] = PointerGetDatum(pk_bytea);
	values[Anum_pg_role_pubkeys_keystring - 1] = CStringGetTextDatum("");
	values[Anum_pg_role_pubkeys_credential_id - 1] = PointerGetDatum(cid_bytea);
	values[Anum_pg_role_pubkeys_rp_id - 1] = CStringGetTextDatum(rp_id);
	values[Anum_pg_role_pubkeys_enrolled_at - 1] = TimestampTzGetDatum(GetCurrentTimestamp());

	/* Create the tuple and insert it */
	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	pfree(pk_bytea);
	pfree(cid_bytea);

	elog(DEBUG1, "PASSKEY: stored new credential for role %u", roleid);
	return true;
}

/*
 * Parse authenticator data from MakeCredential response.
 * The authData includes: rpIdHash(32) + flags(1) + counter(4) + aaguid(16) +
 * credIdLen(2) + credentialId(credIdLen) + publicKey(COSE_Key)
 *
 * Returns 0 on success, -1 on error.
 */
static int
parse_attestation_auth_data(const uint8 *auth_data, int auth_data_len,
							uint8 **credential_id, int *credential_id_len,
							uint8 **public_key, int *public_key_len)
{
	const uint8 *p = auth_data;
	const uint8 *end = auth_data + auth_data_len;
	uint8		flags;
	uint16		cred_id_len;
	int			cose_key_len;

	*credential_id = NULL;
	*credential_id_len = 0;
	*public_key = NULL;
	*public_key_len = 0;

	/* Skip rpIdHash (32 bytes) */
	if (p + 32 > end)
		return -1;
	p += 32;

	/* Read flags */
	if (p + 1 > end)
		return -1;
	flags = *p++;

	/* Check AT flag (attested credential data included) */
	if (!(flags & PASSKEY_FLAG_AT))
	{
		elog(DEBUG1, "PASSKEY: attestation flags missing AT bit");
		return -1;
	}

	/* Skip counter (4 bytes) */
	if (p + 4 > end)
		return -1;
	p += 4;

	/* Skip AAGUID (16 bytes) */
	if (p + 16 > end)
		return -1;
	p += 16;

	/* Read credential ID length (big-endian uint16) */
	if (p + 2 > end)
		return -1;
	cred_id_len = ((uint16) p[0] << 8) | p[1];
	p += 2;

	/* Read credential ID */
	if (p + cred_id_len > end)
		return -1;
	*credential_id = palloc(cred_id_len);
	memcpy(*credential_id, p, cred_id_len);
	*credential_id_len = cred_id_len;
	p += cred_id_len;

	/*
	 * The public key is a COSE_Key encoded in CBOR.
	 * For ES256, it's a map with:
	 *   1 (kty): 2 (EC2)
	 *   3 (alg): -7 (ES256)
	 *  -1 (crv): 1 (P-256)
	 *  -2 (x): bstr(32)
	 *  -3 (y): bstr(32)
	 *
	 * We need to extract x and y to build the uncompressed point (04 || x || y).
	 * For simplicity, we'll parse just enough to extract x and y coordinates.
	 */
	{
		const uint8 *cose_start = p;
		uint8		x_coord[32];
		uint8		y_coord[32];
		bool		found_x = false;
		bool		found_y = false;

		/* Remaining bytes are the COSE_Key */
		cose_key_len = end - p;
		if (cose_key_len < 10)
			return -1;

		/*
		 * Simple CBOR parsing for COSE_Key.
		 * We look for keys -2 (x) and -3 (y) which are encoded as 0x21 and 0x22.
		 */
		while (p < end - 34)
		{
			uint8		key = *p++;

			/* Key -2 (x coordinate) encoded as CBOR negative int: 0x21 */
			if (key == 0x21)
			{
				/* Next should be bytes header for 32-byte value */
				if (*p == 0x58 && p[1] == 32)
				{
					p += 2;
					memcpy(x_coord, p, 32);
					p += 32;
					found_x = true;
				}
			}
			/* Key -3 (y coordinate) encoded as CBOR negative int: 0x22 */
			else if (key == 0x22)
			{
				if (*p == 0x58 && p[1] == 32)
				{
					p += 2;
					memcpy(y_coord, p, 32);
					p += 32;
					found_y = true;
				}
			}

			if (found_x && found_y)
				break;
		}

		if (!found_x || !found_y)
		{
			pfree(*credential_id);
			*credential_id = NULL;
			return -1;
		}

		/* Build uncompressed EC point: 04 || x || y */
		*public_key = palloc(65);
		(*public_key)[0] = 0x04;
		memcpy(*public_key + 1, x_coord, 32);
		memcpy(*public_key + 33, y_coord, 32);
		*public_key_len = 65;
	}

	return 0;
}

static int
passkey_exchange(void *opaq, const char *input, int inputlen,
				 char **output, int *outputlen, const char **logdetail)
{
	passkey_state *st = (passkey_state *) opaq;

	*output = NULL;
	*outputlen = 0;
	*logdetail = NULL;

	if (st->state == PASSKEY_STATE_INIT)
	{
		/*
		 * Client-first message: empty or contains a credential_id hint.
		 *
		 * In TOFU mode, we first request the password, then decide whether
		 * to do GetAssertion (credential exists) or MakeCredential (no credential).
		 */
		StringInfoData buf;

		elog(DEBUG1, "PASSKEY: received client-first-message (%d bytes)", inputlen);

		/*
		 * Get the password verifier for this role.
		 * Passkey auth requires a password to be set (for TOFU).
		 */
		if (!st->doomed)
		{
			st->shadow_pass = get_role_password(st->port->user_name, &st->logdetail);
			if (!st->shadow_pass)
			{
				st->doomed = true;
				if (!st->logdetail)
					st->logdetail = psprintf("role \"%s\" has no password", st->port->user_name);
			}
		}

		/*
		 * Send password request message:
		 * msg_type(1) + version(1)
		 */
		initStringInfo(&buf);
		appendStringInfoChar(&buf, PASSKEY_MSG_PASSWORD_REQUEST);
		appendStringInfoChar(&buf, PASSKEY_PROTOCOL_VERSION);

		*output = buf.data;
		*outputlen = buf.len;

		elog(DEBUG1, "PASSKEY: requesting password");
		st->state = PASSKEY_STATE_PASSWORD_REQUESTED;
		return PG_SASL_EXCHANGE_CONTINUE;
	}

	if (st->state == PASSKEY_STATE_PASSWORD_REQUESTED)
	{
		/*
		 * Receive password response:
		 * msg_type(1) + password_len(2) + password
		 */
		const uint8 *p = (const uint8 *) input;
		const uint8 *end = p + inputlen;
		uint16		pass_len;
		char	   *password;
		CatCList   *memlist;
		int			i;

		if (p + 1 > end || p[0] != PASSKEY_MSG_PASSWORD_RESPONSE)
		{
			*logdetail = "expected password response";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		p++;

		if (p + 2 > end)
		{
			*logdetail = "malformed password response";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		pass_len = ((uint16) p[0] << 8) | p[1];
		p += 2;

		if (p + pass_len > end)
		{
			*logdetail = "malformed password response";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		password = palloc(pass_len + 1);
		memcpy(password, p, pass_len);
		password[pass_len] = '\0';

		elog(DEBUG1, "PASSKEY: received password (%u bytes)", pass_len);

		/*
		 * Verify password against stored verifier.
		 * This uses the same verification as plain password auth.
		 */
		if (!st->doomed)
		{
			int			result;

			result = plain_crypt_verify(st->port->user_name, st->shadow_pass,
										password, &st->logdetail);
			if (result != STATUS_OK)
			{
				st->doomed = true;
				if (!st->logdetail)
					st->logdetail = "password verification failed";
			}
		}

		/* Clear password from memory */
		explicit_bzero(password, pass_len);
		pfree(password);

		if (st->doomed)
		{
			elog(DEBUG1, "PASSKEY: password verification failed");
			/* Continue to avoid timing attacks, but will fail later */
		}
		else
		{
			elog(DEBUG1, "PASSKEY: password verified successfully");
		}

		/*
		 * Look up WebAuthn credentials for this user to decide between
		 * GetAssertion (credential exists) and MakeCredential (no credential).
		 * pg_role_pubkeys is a shared catalog, so it's accessible during auth.
		 */
		st->is_registration = true;		/* Default to registration */
		if (!st->doomed)
		{
			memlist = SearchSysCacheList1(ROLEPUBKEYSROLEID, ObjectIdGetDatum(st->roleid));
			for (i = 0; i < memlist->n_members; i++)
			{
				HeapTuple	tuple = &memlist->members[i]->tuple;
				Form_pg_role_pubkeys pk = (Form_pg_role_pubkeys) GETSTRUCT(tuple);
				Datum		d;
				bool		isnull;

				/* Only consider WebAuthn credentials */
				if (pk->credential_type != CRED_TYPE_WEBAUTHN)
					continue;

				/* Check rp_id matches */
				d = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple, Anum_pg_role_pubkeys_rp_id, &isnull);
				if (!isnull)
				{
					char	   *stored_rp_id = TextDatumGetCString(d);

					if (strcmp(stored_rp_id, st->rp_id) != 0)
					{
						pfree(stored_rp_id);
						continue;
					}
					pfree(stored_rp_id);
				}

				/* Found a matching credential - this is authentication, not registration */
				st->is_registration = false;

				/* Get credential_id */
				d = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple, Anum_pg_role_pubkeys_credential_id, &isnull);
				if (!isnull)
				{
					bytea	   *cid = DatumGetByteaP(d);

					st->credential_id_len = VARSIZE_ANY_EXHDR(cid);
					st->credential_id = palloc(st->credential_id_len);
					memcpy(st->credential_id, VARDATA_ANY(cid), st->credential_id_len);
				}

				/* Get public key */
				d = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple, Anum_pg_role_pubkeys_public_key, &isnull);
				if (!isnull)
				{
					bytea	   *pk_bytes = DatumGetByteaP(d);

					st->public_key_len = VARSIZE_ANY_EXHDR(pk_bytes);
					st->public_key = palloc(st->public_key_len);
					memcpy(st->public_key, VARDATA_ANY(pk_bytes), st->public_key_len);
				}

				st->cred_oid = pk->oid;
				st->key_name = pstrdup(NameStr(pk->key_name));
				st->algorithm = pk->algorithm;

				elog(DEBUG1, "PASSKEY: found existing credential for role %u (cred_id_len=%d)",
					 st->roleid, st->credential_id_len);
				break;
			}
			ReleaseSysCacheList(memlist);
		}

		if (st->is_registration)
			elog(DEBUG1, "PASSKEY: no existing credential found, will do registration");

		/* Generate random challenge */
		if (!pg_strong_random(st->challenge, PASSKEY_CHALLENGE_LENGTH))
			elog(ERROR, "could not generate random challenge");

		/*
		 * Build passkey challenge message:
		 * msg_type(1) + version(1) + operation(1) + challenge(32) +
		 * rp_id_len(2) + rp_id + options(1) + cred_id_len(2) + cred_id +
		 * [for registration: user_id_len(2) + user_id + user_name_len(2) + user_name]
		 */
		{
			StringInfoData buf;
			uint8		opts = PASSKEY_OPT_REQUIRE_UP;
			size_t		rp_id_len = strlen(st->rp_id);

			if (st->require_uv)
				opts |= PASSKEY_OPT_REQUIRE_UV;

			initStringInfo(&buf);
			appendStringInfoChar(&buf, PASSKEY_MSG_PASSKEY_CHALLENGE);
			appendStringInfoChar(&buf, PASSKEY_PROTOCOL_VERSION);
			appendStringInfoChar(&buf, st->is_registration ? PASSKEY_OP_MAKE_CREDENTIAL : PASSKEY_OP_GET_ASSERTION);
			appendBinaryStringInfo(&buf, (char *) st->challenge, PASSKEY_CHALLENGE_LENGTH);
			appendStringInfoChar(&buf, (rp_id_len >> 8) & 0xFF);
			appendStringInfoChar(&buf, rp_id_len & 0xFF);
			appendBinaryStringInfo(&buf, st->rp_id, rp_id_len);
			appendStringInfoChar(&buf, opts);

			/* Append credential_id if we have one (for allowCredentials) */
			if (st->credential_id && st->credential_id_len > 0)
			{
				appendStringInfoChar(&buf, (st->credential_id_len >> 8) & 0xFF);
				appendStringInfoChar(&buf, st->credential_id_len & 0xFF);
				appendBinaryStringInfo(&buf, (char *) st->credential_id, st->credential_id_len);
			}
			else
			{
				appendStringInfoChar(&buf, 0);
				appendStringInfoChar(&buf, 0);
			}

			/* For registration, include user info */
			if (st->is_registration)
			{
				uint8		user_id[16];
				int			user_id_len = 0;
				size_t		user_name_len = strlen(st->port->user_name);

				generate_user_id(st->port->user_name, user_id, &user_id_len);

				/* Store user_id for later verification */
				st->user_id = palloc(user_id_len);
				memcpy(st->user_id, user_id, user_id_len);
				st->user_id_len = user_id_len;

				appendStringInfoChar(&buf, (user_id_len >> 8) & 0xFF);
				appendStringInfoChar(&buf, user_id_len & 0xFF);
				appendBinaryStringInfo(&buf, (char *) user_id, user_id_len);
				appendStringInfoChar(&buf, (user_name_len >> 8) & 0xFF);
				appendStringInfoChar(&buf, user_name_len & 0xFF);
				appendBinaryStringInfo(&buf, st->port->user_name, user_name_len);

				elog(DEBUG1, "PASSKEY: sending MakeCredential challenge (rp_id=%s, user=%s)",
					 st->rp_id, st->port->user_name);
			}
			else
			{
				elog(DEBUG1, "PASSKEY: sending GetAssertion challenge (rp_id=%s, options=0x%02x)",
					 st->rp_id, opts);
			}

			*output = buf.data;
			*outputlen = buf.len;
		}

		st->state = PASSKEY_STATE_PASSWORD_VERIFIED;
		return PG_SASL_EXCHANGE_CONTINUE;
	}

	if (st->state == PASSKEY_STATE_PASSWORD_VERIFIED)
	{
		/*
		 * Receive passkey response.
		 * msg_type(1) + response_data...
		 *
		 * For GetAssertion:
		 *   authenticator_data_len(2) + authenticator_data +
		 *   client_data_json_len(2) + client_data_json +
		 *   signature_len(2) + signature +
		 *   credential_id_len(2) + credential_id
		 *
		 * For MakeCredential:
		 *   authenticator_data_len(2) + authenticator_data +
		 *   client_data_json_len(2) + client_data_json +
		 *   credential_id_len(2) + credential_id +
		 *   public_key_len(2) + public_key
		 */
		const uint8 *p = (const uint8 *) input;
		const uint8 *end = p + inputlen;
		uint8		msg_type;

		if (p + 1 > end)
		{
			*logdetail = "malformed passkey response";
			return PG_SASL_EXCHANGE_FAILURE;
		}
		msg_type = *p++;

		if (msg_type != PASSKEY_MSG_PASSKEY_RESPONSE)
		{
			*logdetail = "expected passkey response";
			return PG_SASL_EXCHANGE_FAILURE;
		}

		if (st->is_registration)
		{
			/*
			 * Parse MakeCredential response and store the new credential.
			 */
			uint16		auth_data_len, client_data_len, cred_id_len, pubkey_len;
			const uint8 *auth_data, *client_data_json, *cred_id, *pubkey;
			uint8		rp_hash[PG_SHA256_DIGEST_LENGTH];
			pg_cryptohash_ctx *ctx;

			/* Parse authenticator_data */
			if (p + 2 > end)
				goto parse_error;
			auth_data_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + auth_data_len > end || auth_data_len < PASSKEY_AUTH_DATA_MIN_LENGTH)
				goto parse_error;
			auth_data = p;
			p += auth_data_len;

			/* Parse client_data_json */
			if (p + 2 > end)
				goto parse_error;
			client_data_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + client_data_len > end)
				goto parse_error;
			client_data_json = p;
			p += client_data_len;

			/* Parse credential_id */
			if (p + 2 > end)
				goto parse_error;
			cred_id_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + cred_id_len > end)
				goto parse_error;
			cred_id = p;
			p += cred_id_len;

			/* Parse public_key */
			if (p + 2 > end)
				goto parse_error;
			pubkey_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + pubkey_len > end)
				goto parse_error;
			pubkey = p;

			elog(DEBUG1, "PASSKEY: received attestation (auth_data=%u, client_data=%u, cred_id=%u, pubkey=%u)",
				 auth_data_len, client_data_len, cred_id_len, pubkey_len);

			/* Verify rpIdHash in authenticator data matches our rp_id */
			ctx = pg_cryptohash_create(PG_SHA256);
			if (!ctx || pg_cryptohash_init(ctx) < 0 ||
				pg_cryptohash_update(ctx, (uint8 *) st->rp_id, strlen(st->rp_id)) < 0 ||
				pg_cryptohash_final(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0)
			{
				pg_cryptohash_free(ctx);
				*logdetail = "hash computation failed";
				return PG_SASL_EXCHANGE_FAILURE;
			}
			pg_cryptohash_free(ctx);

			if (memcmp(auth_data, rp_hash, 32) != 0)
			{
				*logdetail = "rpIdHash mismatch in attestation";
				return PG_SASL_EXCHANGE_FAILURE;
			}

			if (st->doomed)
			{
				elog(DEBUG1, "PASSKEY: registration failed (password was invalid)");
				*logdetail = st->logdetail;
				return PG_SASL_EXCHANGE_FAILURE;
			}

			/*
			 * Defer credential storage until after database init.
			 * We can't access catalogs yet because MyDatabaseId isn't set.
			 * Store the credential info in TopMemoryContext so it survives.
			 */
			pending_cred.pending = true;
			pending_cred.roleid = st->roleid;
			pending_cred.rp_id = MemoryContextStrdup(TopMemoryContext, st->rp_id);
			pending_cred.credential_id_len = cred_id_len;
			pending_cred.credential_id = MemoryContextAlloc(TopMemoryContext, cred_id_len);
			memcpy(pending_cred.credential_id, cred_id, cred_id_len);
			pending_cred.public_key_len = pubkey_len;
			pending_cred.public_key = MemoryContextAlloc(TopMemoryContext, pubkey_len);
			memcpy(pending_cred.public_key, pubkey, pubkey_len);

			elog(DEBUG1, "PASSKEY: registration succeeded, credential storage deferred");
			st->state = PASSKEY_STATE_FINISHED;
			return PG_SASL_EXCHANGE_SUCCESS;
		}
		else
		{
			/*
			 * Parse GetAssertion response and verify signature.
			 */
			uint16		auth_data_len, client_data_len, sig_len, cred_id_len;
			const uint8 *auth_data, *client_data_json, *signature, *cred_id;
			uint8		sig_flags;
			uint8		rp_hash[PG_SHA256_DIGEST_LENGTH];
			uint8		client_hash[PG_SHA256_DIGEST_LENGTH];
			uint8		signed_hash[PG_SHA256_DIGEST_LENGTH];
			pg_cryptohash_ctx *ctx;

			/* Parse authenticator_data */
			if (p + 2 > end)
				goto parse_error;
			auth_data_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + auth_data_len > end || auth_data_len < PASSKEY_AUTH_DATA_MIN_LENGTH)
				goto parse_error;
			auth_data = p;
			p += auth_data_len;

			/* Parse client_data_json */
			if (p + 2 > end)
				goto parse_error;
			client_data_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + client_data_len > end)
				goto parse_error;
			client_data_json = p;
			p += client_data_len;

			/* Parse signature */
			if (p + 2 > end)
				goto parse_error;
			sig_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + sig_len > end)
				goto parse_error;
			signature = p;
			p += sig_len;

			/* Parse credential_id */
			if (p + 2 > end)
				goto parse_error;
			cred_id_len = ((uint16) p[0] << 8) | p[1];
			p += 2;
			if (p + cred_id_len > end)
				goto parse_error;
			cred_id = p;

			elog(DEBUG1, "PASSKEY: received assertion (auth_data=%u, client_data=%u, sig=%u, cred_id=%u)",
				 auth_data_len, client_data_len, sig_len, cred_id_len);

			/* Extract flags from authenticator data (byte 32, after rpIdHash) */
			sig_flags = auth_data[32];

			if (!(sig_flags & PASSKEY_FLAG_UP))
			{
				*logdetail = "user presence not verified";
				return PG_SASL_EXCHANGE_FAILURE;
			}

			if (st->require_uv && !(sig_flags & PASSKEY_FLAG_UV))
			{
				*logdetail = "user verification required";
				return PG_SASL_EXCHANGE_FAILURE;
			}

			/* Verify rpIdHash in authenticator data matches our rp_id */
			ctx = pg_cryptohash_create(PG_SHA256);
			if (!ctx || pg_cryptohash_init(ctx) < 0 ||
				pg_cryptohash_update(ctx, (uint8 *) st->rp_id, strlen(st->rp_id)) < 0 ||
				pg_cryptohash_final(ctx, rp_hash, PG_SHA256_DIGEST_LENGTH) < 0)
			{
				pg_cryptohash_free(ctx);
				*logdetail = "hash computation failed";
				return PG_SASL_EXCHANGE_FAILURE;
			}
			pg_cryptohash_free(ctx);

			if (memcmp(auth_data, rp_hash, 32) != 0)
			{
				*logdetail = "rpIdHash mismatch";
				return PG_SASL_EXCHANGE_FAILURE;
			}

			/*
			 * Compute clientDataHash = SHA256(clientDataJSON)
			 */
			ctx = pg_cryptohash_create(PG_SHA256);
			if (!ctx || pg_cryptohash_init(ctx) < 0 ||
				pg_cryptohash_update(ctx, client_data_json, client_data_len) < 0 ||
				pg_cryptohash_final(ctx, client_hash, PG_SHA256_DIGEST_LENGTH) < 0)
			{
				pg_cryptohash_free(ctx);
				*logdetail = "hash computation failed";
				return PG_SASL_EXCHANGE_FAILURE;
			}
			pg_cryptohash_free(ctx);

			/*
			 * Compute signedDataHash = SHA256(authenticatorData || clientDataHash)
			 */
			ctx = pg_cryptohash_create(PG_SHA256);
			if (!ctx || pg_cryptohash_init(ctx) < 0 ||
				pg_cryptohash_update(ctx, auth_data, auth_data_len) < 0 ||
				pg_cryptohash_update(ctx, client_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
				pg_cryptohash_final(ctx, signed_hash, PG_SHA256_DIGEST_LENGTH) < 0)
			{
				pg_cryptohash_free(ctx);
				*logdetail = "hash computation failed";
				return PG_SASL_EXCHANGE_FAILURE;
			}
			pg_cryptohash_free(ctx);

			/* Verify signature */
			{
				PasskeyVerifyResult verify_result;

				if (st->doomed)
				{
					elog(DEBUG1, "PASSKEY: authentication failed (password was invalid)");
					*logdetail = st->logdetail;
					return PG_SASL_EXCHANGE_FAILURE;
				}

				if (st->algorithm != COSE_ALG_ES256)
				{
					*logdetail = "unsupported algorithm";
					return PG_SASL_EXCHANGE_FAILURE;
				}

				verify_result = passkey_verify_es256(st->public_key, signed_hash,
													 signature, sig_len);

				if (verify_result != PASSKEY_VERIFY_OK)
				{
					elog(DEBUG1, "PASSKEY: signature verification failed (result=%d)", verify_result);
					*logdetail = "signature verification failed";
					return PG_SASL_EXCHANGE_FAILURE;
				}

				/* Log successful verification details */
				elog(DEBUG1, "PASSKEY: ES256 signature verified successfully");
				elog(DEBUG1, "PASSKEY: verified assertion - cred_id_len=%u, flags=0x%02x (UP=%d, UV=%d), rp_id=%s",
					 cred_id_len, sig_flags,
					 (sig_flags & PASSKEY_FLAG_UP) ? 1 : 0,
					 (sig_flags & PASSKEY_FLAG_UV) ? 1 : 0,
					 st->rp_id);
			}

			elog(DEBUG1, "PASSKEY: authentication succeeded (password + passkey verified)");
			st->state = PASSKEY_STATE_FINISHED;
			return PG_SASL_EXCHANGE_SUCCESS;
		}

parse_error:
		*logdetail = "malformed passkey response";
		return PG_SASL_EXCHANGE_FAILURE;
	}

	return PG_SASL_EXCHANGE_FAILURE;
}

/*
 * Store any pending passkey credential that was deferred during authentication.
 * This should be called after database initialization is complete (i.e., after
 * RelationCacheInitializePhase3() in postinit.c) when catalog access is available.
 */
void
passkey_store_pending_credential(void)
{
	if (!pending_cred.pending)
		return;

	pending_cred.pending = false;

	elog(DEBUG1, "PASSKEY: storing deferred credential for role %u", pending_cred.roleid);

	/* Now we can access catalogs safely */
	store_passkey_credential(pending_cred.roleid, pending_cred.rp_id,
							 pending_cred.credential_id, pending_cred.credential_id_len,
							 pending_cred.public_key, pending_cred.public_key_len);

	/* Clean up */
	pfree(pending_cred.rp_id);
	pfree(pending_cred.credential_id);
	pfree(pending_cred.public_key);
	memset(&pending_cred, 0, sizeof(pending_cred));

	elog(DEBUG1, "PASSKEY: stored deferred credential successfully");
}

#endif							/* USE_OPENSSL */
