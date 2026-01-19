/*-------------------------------------------------------------------------
 *
 * auth-fido2.c
 *	  Server-side implementation of FIDO2/WebAuthn SASL authentication.
 *
 * This implements a SASL mechanism named "FIDO2" for authenticating users
 * with hardware security keys (e.g., YubiKey) or platform authenticators
 * (e.g., macOS Secure Enclave).
 *
 * The authentication flow:
 * 1. Server generates a random challenge
 * 2. Server sends challenge + relying party ID to client
 * 3. Client discovers resident credentials on the authenticator matching the
 *    relying party ID, then signs the challenge using the security key
 * 4. Server verifies the signature using dual verification (micro-ecc + bearssl)
 *
 * Note: Only resident (discoverable) credentials are currently supported.
 * The client discovers credentials stored on the authenticator rather than
 * using server-provided credential IDs as key handles.
 *
 * See src/include/libpq/fido2.h for protocol details.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/auth-fido2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/pg_role_pubkeys.h"
#include "commands/user.h"
#include "common/cryptohash.h"
#include "common/fido2-cbor/fido2_cbor.h"
#include "common/fido2-verify.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "libpq/fido2.h"
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

/* Forward declarations */
static void fido2_get_mechanisms(Port *port, StringInfo buf);
static void *fido2_init(Port *port, const char *selected_mech,
						const char *shadow_pass);
static int	fido2_exchange(void *opaq, const char *input, int inputlen,
						   char **output, int *outputlen,
						   const char **logdetail);

/* Mechanism declaration */
const pg_be_sasl_mech pg_be_fido2_mech = {
	fido2_get_mechanisms,
	fido2_init,
	fido2_exchange,
	FIDO2_MAX_ASSERTION_MSG
};

/*
 * State for a FIDO2 authentication exchange
 */
typedef enum
{
	FIDO2_AUTH_INIT,
	FIDO2_AUTH_CHALLENGE_SENT,
	FIDO2_AUTH_FINISHED
} fido2_state_enum;

/*
 * Stored credential info loaded from pg_role_pubkeys
 */
typedef struct Fido2Credential
{
	Oid			oid;
	uint8	   *credential_id;
	int			credential_id_len;
	char	   *key_name;
	int16		algorithm;
	uint8	   *public_key;
	int			public_key_len;
	int64		sign_count;
} Fido2Credential;

typedef struct
{
	fido2_state_enum state;

	Port	   *port;
	Oid			roleid;

	/* Challenge sent to client */
	uint8		challenge[FIDO2_CHALLENGE_LENGTH];

	/* Relying party ID (hostname or configured value) */
	char	   *rp_id;

	/* Registered credentials for this user */
	Fido2Credential *credentials;
	int			num_credentials;

	/* Options from HBA */
	bool		require_uv;

	/* If doomed, we continue mock authentication */
	bool		doomed;
	char	   *logdetail;
} fido2_state;

/* Helper functions */
static void load_user_credentials(fido2_state *state);
static char *build_challenge_message(fido2_state *state, int *len);
static bool verify_assertion(fido2_state *state, const char *input, int inputlen,
							 const char **logdetail);
static void update_sign_count(Oid credential_oid, int64 new_count);

/*
 * Get list of SASL mechanisms supported
 */
static void
fido2_get_mechanisms(Port *port, StringInfo buf)
{
	appendStringInfoString(buf, FIDO2_MECHANISM_NAME);
	appendStringInfoChar(buf, '\0');
}

/*
 * Initialize FIDO2 authentication state
 */
static void *
fido2_init(Port *port, const char *selected_mech, const char *shadow_pass)
{
	fido2_state *state;

	state = (fido2_state *) palloc0(sizeof(fido2_state));
	state->state = FIDO2_AUTH_INIT;
	state->port = port;
	state->doomed = false;

	/* Look up the user */
	state->roleid = get_role_oid(port->user_name, true);
	elog(DEBUG1, "FIDO2: authenticating user \"%s\"", port->user_name);
	if (!OidIsValid(state->roleid))
	{
		state->doomed = true;
		state->logdetail = psprintf("Role \"%s\" does not exist",
									port->user_name);
	}

	/* Load registered credentials from pg_role_pubkeys */
	if (!state->doomed)
	{
		load_user_credentials(state);
		elog(DEBUG1, "FIDO2: loaded %d credential(s) for user \"%s\"",
			 state->num_credentials, port->user_name);
		if (state->num_credentials == 0)
		{
			state->doomed = true;
			state->logdetail = psprintf("Role \"%s\" has no FIDO2 credentials",
										port->user_name);
		}
	}

	/* Generate challenge */
	if (!pg_strong_random(state->challenge, FIDO2_CHALLENGE_LENGTH))
		elog(ERROR, "could not generate random FIDO2 challenge");
	elog(DEBUG1, "FIDO2: generated %d-byte challenge", FIDO2_CHALLENGE_LENGTH);

	/*
	 * Determine the relying party ID.
	 * Use the application from the first registered credential, since OpenSSH SK keys
	 * embed the application they were registered with. This ensures the rp_id matches
	 * what the security key expects for signing.
	 */
	if (state->num_credentials > 0 && state->credentials[0].credential_id_len > 0)
	{
		/* credential_id contains the application string from the OpenSSH key */
		state->rp_id = pnstrdup((char *) state->credentials[0].credential_id,
								state->credentials[0].credential_id_len);
	}
	else
	{
		state->rp_id = pstrdup("localhost");	/* fallback */
	}
	elog(DEBUG1, "FIDO2: rp_id set to \"%s\" (credential_id_len=%d)",
		 state->rp_id,
		 state->num_credentials > 0 ? state->credentials[0].credential_id_len : 0);

	/* Get options from HBA line */
	state->require_uv = false;	/* TODO: get from port->hba */

	return state;
}

/*
 * Exchange FIDO2 messages
 */
static int
fido2_exchange(void *opaq, const char *input, int inputlen,
			   char **output, int *outputlen,
			   const char **logdetail)
{
	fido2_state *state = (fido2_state *) opaq;

	*output = NULL;
	*outputlen = 0;
	*logdetail = NULL;

	elog(DEBUG1, "FIDO2: exchange state=%d, inputlen=%d", state->state, inputlen);

	switch (state->state)
	{
		case FIDO2_AUTH_INIT:
			/* First message from client - just the mechanism name */
			/* Send challenge to client */
			*output = build_challenge_message(state, outputlen);
			elog(DEBUG1, "FIDO2: sending challenge message (%d bytes, %d credentials)",
				 *outputlen, state->num_credentials);
			state->state = FIDO2_AUTH_CHALLENGE_SENT;
			return PG_SASL_EXCHANGE_CONTINUE;

		case FIDO2_AUTH_CHALLENGE_SENT:
			/* Client sent assertion response */
			if (state->doomed)
			{
				*logdetail = state->logdetail;
				return PG_SASL_EXCHANGE_FAILURE;
			}

			elog(DEBUG1, "FIDO2: received assertion (%d bytes), verifying", inputlen);
			if (!verify_assertion(state, input, inputlen, logdetail))
			{
				return PG_SASL_EXCHANGE_FAILURE;
			}

			state->state = FIDO2_AUTH_FINISHED;
			return PG_SASL_EXCHANGE_SUCCESS;

		case FIDO2_AUTH_FINISHED:
			elog(ERROR, "FIDO2 exchange already finished");
			break;
	}

	/* Should not reach here */
	return PG_SASL_EXCHANGE_FAILURE;
}

/*
 * Load user's registered FIDO2 credentials from pg_role_pubkeys
 *
 * Uses syscache instead of table scans because this runs during authentication,
 * before a database has been selected. Syscache has special handling for
 * pre-database-selected access, while table_open() would fail with
 * "cannot read pg_class without having selected a database".
 */
static void
load_user_credentials(fido2_state *state)
{
	CatCList   *memlist;
	List	   *cred_list = NIL;
	ListCell   *lc;
	int			i;

	/* Use syscache to get all credentials for this role */
	elog(DEBUG1, "FIDO2: searching pg_role_pubkeys for role OID %u", state->roleid);
	memlist = SearchSysCacheList1(ROLEPUBKEYSROLEID,
								  ObjectIdGetDatum(state->roleid));

	for (i = 0; i < memlist->n_members; i++)
	{
		HeapTuple	tuple = &memlist->members[i]->tuple;
		Form_pg_role_pubkeys pubkey = (Form_pg_role_pubkeys) GETSTRUCT(tuple);
		Fido2Credential *cred;
		Datum		datum;
		bool		isnull;
		bytea	   *credential_id_bytea;
		bytea	   *public_key_bytea;

		cred = (Fido2Credential *) palloc0(sizeof(Fido2Credential));
		cred->oid = pubkey->oid;
		cred->key_name = pstrdup(NameStr(pubkey->key_name));
		cred->algorithm = pubkey->algorithm;
		cred->sign_count = pubkey->sign_count;

		/* Get credential_id (bytea, variable length) */
		datum = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple,
								Anum_pg_role_pubkeys_credential_id, &isnull);
		if (!isnull)
		{
			credential_id_bytea = DatumGetByteaP(datum);
			cred->credential_id_len = VARSIZE_ANY_EXHDR(credential_id_bytea);
			cred->credential_id = palloc(cred->credential_id_len);
			memcpy(cred->credential_id, VARDATA_ANY(credential_id_bytea),
				   cred->credential_id_len);
		}

		/* Get public_key (bytea, variable length) */
		datum = SysCacheGetAttr(ROLEPUBKEYSROLEID, tuple,
								Anum_pg_role_pubkeys_public_key, &isnull);
		if (!isnull)
		{
			public_key_bytea = DatumGetByteaP(datum);
			cred->public_key_len = VARSIZE_ANY_EXHDR(public_key_bytea);
			cred->public_key = palloc(cred->public_key_len);
			memcpy(cred->public_key, VARDATA_ANY(public_key_bytea),
				   cred->public_key_len);
		}

		elog(DEBUG1, "FIDO2: loaded credential \"%s\" (algorithm=%d, credential_id_len=%d)",
			 cred->key_name, cred->algorithm, cred->credential_id_len);
		cred_list = lappend(cred_list, cred);
	}

	ReleaseSysCacheList(memlist);

	/* Convert list to array */
	state->num_credentials = list_length(cred_list);
	if (state->num_credentials > 0)
	{
		state->credentials = palloc(sizeof(Fido2Credential) * state->num_credentials);
		i = 0;
		foreach(lc, cred_list)
		{
			Fido2Credential *cred = (Fido2Credential *) lfirst(lc);

			memcpy(&state->credentials[i], cred, sizeof(Fido2Credential));
			i++;
		}
	}
}

/*
 * Build the challenge message to send to the client
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
static char *
build_challenge_message(fido2_state *state, int *len)
{
	StringInfoData buf;
	uint16		rp_id_len;
	uint8		options;
	int			i;

	initStringInfo(&buf);

	/* Protocol version */
	appendStringInfoChar(&buf, FIDO2_PROTOCOL_VERSION);

	/* Challenge */
	appendBinaryStringInfo(&buf, (const char *) state->challenge,
						   FIDO2_CHALLENGE_LENGTH);

	/* Relying party ID */
	rp_id_len = strlen(state->rp_id) + 1;	/* Include null terminator */
	rp_id_len = pg_hton16(rp_id_len);
	appendBinaryStringInfo(&buf, (const char *) &rp_id_len, 2);
	appendBinaryStringInfo(&buf, state->rp_id, strlen(state->rp_id) + 1);

	/* Number of credentials */
	appendStringInfoChar(&buf, (char) state->num_credentials);

	/* Credential IDs */
	for (i = 0; i < state->num_credentials; i++)
	{
		uint16		cred_len = pg_hton16(state->credentials[i].credential_id_len);

		appendBinaryStringInfo(&buf, (const char *) &cred_len, 2);
		appendBinaryStringInfo(&buf, (const char *) state->credentials[i].credential_id,
							   state->credentials[i].credential_id_len);
	}

	/* Options */
	options = FIDO2_OPT_REQUIRE_UP;
	if (state->require_uv)
		options |= FIDO2_OPT_REQUIRE_UV;
	appendStringInfoChar(&buf, options);

	elog(DEBUG1, "FIDO2: building challenge message: rp_id=\"%s\", options=0x%02x",
		 state->rp_id, options);

	*len = buf.len;
	return buf.data;
}

/*
 * Parse and verify the assertion from the client
 *
 * Assertion format:
 *   credential_id_len: 2 bytes (big-endian)
 *   credential_id: credential_id_len bytes
 *   authenticator_data_len: 2 bytes (big-endian)
 *   authenticator_data: authenticator_data_len bytes
 *   signature_len: 2 bytes (big-endian)
 *   signature: signature_len bytes (raw format: R || S, 64 bytes for ES256)
 */
static bool
verify_assertion(fido2_state *state, const char *input, int inputlen,
				 const char **logdetail)
{
	const uint8 *p = (const uint8 *) input;
	const uint8 *end = p + inputlen;
	uint16		credential_id_len;
	const uint8 *credential_id;
	uint16		auth_data_len;
	const uint8 *auth_data;
	uint16		signature_len;
	const uint8 *signature;
	Fido2Credential *matched_cred = NULL;
	Fido2AuthData parsed_auth_data;
	const char *parse_error;
	uint8		client_data_hash[PG_SHA256_DIGEST_LENGTH];
	uint8		signed_data_hash[PG_SHA256_DIGEST_LENGTH];
	pg_cryptohash_ctx *sha256_ctx;
	int			i;
	Fido2VerifyResult verify_result;

	/* Parse credential_id */
	if (end - p < 2)
	{
		*logdetail = "assertion too short: missing credential_id length";
		return false;
	}
	credential_id_len = pg_ntoh16(*(const uint16 *) p);
	p += 2;

	if (end - p < credential_id_len)
	{
		*logdetail = "assertion too short: credential_id truncated";
		return false;
	}
	credential_id = p;
	p += credential_id_len;
	elog(DEBUG1, "FIDO2: parsed credential_id (%d bytes)", credential_id_len);

	/* Parse authenticator_data */
	if (end - p < 2)
	{
		*logdetail = "assertion too short: missing authenticator_data length";
		return false;
	}
	auth_data_len = pg_ntoh16(*(const uint16 *) p);
	p += 2;

	if (end - p < auth_data_len)
	{
		*logdetail = "assertion too short: authenticator_data truncated";
		return false;
	}
	auth_data = p;
	p += auth_data_len;
	elog(DEBUG1, "FIDO2: parsed authenticator_data (%d bytes)", auth_data_len);

	/* Parse signature */
	if (end - p < 2)
	{
		*logdetail = "assertion too short: missing signature length";
		return false;
	}
	signature_len = pg_ntoh16(*(const uint16 *) p);
	p += 2;

	if (end - p < signature_len)
	{
		*logdetail = "assertion too short: signature truncated";
		return false;
	}
	signature = p;
	elog(DEBUG1, "FIDO2: parsed signature (%d bytes)", signature_len);

	/* Find matching credential */
	elog(DEBUG1, "FIDO2: searching %d credential(s) for match", state->num_credentials);
	for (i = 0; i < state->num_credentials; i++)
	{
		if (state->credentials[i].credential_id_len == credential_id_len &&
			memcmp(state->credentials[i].credential_id, credential_id,
				   credential_id_len) == 0)
		{
			matched_cred = &state->credentials[i];
			elog(DEBUG1, "FIDO2: matched credential at index %d (algorithm=%d)",
				 i, matched_cred->algorithm);
			break;
		}
	}

	if (matched_cred == NULL)
	{
		*logdetail = "credential ID not found for user";
		return false;
	}

	/* Only ES256 is supported */
	if (matched_cred->algorithm != COSE_ALG_ES256)
	{
		*logdetail = "unsupported credential algorithm";
		return false;
	}

	/* Verify signature is correct length for ES256 */
	if (signature_len != 64)
	{
		*logdetail = "invalid signature length for ES256";
		return false;
	}

	/* Parse authenticator data */
	if (!fido2_parse_auth_data(auth_data, auth_data_len, &parsed_auth_data,
							   &parse_error))
	{
		*logdetail = psprintf("failed to parse authenticator data: %s",
							  parse_error);
		return false;
	}
	elog(DEBUG1, "FIDO2: auth_data flags=0x%02x, sign_count=%u",
		 parsed_auth_data.flags, parsed_auth_data.sign_count);

	/* Check user present flag */
	elog(DEBUG1, "FIDO2: user_present=%d, user_verified=%d",
		 (parsed_auth_data.flags & FIDO2_FLAG_UP) != 0,
		 (parsed_auth_data.flags & FIDO2_FLAG_UV) != 0);
	if (!(parsed_auth_data.flags & FIDO2_FLAG_UP))
	{
		*logdetail = "user present flag not set";
		return false;
	}

	/* Check user verified flag if required */
	if (state->require_uv && !(parsed_auth_data.flags & FIDO2_FLAG_UV))
	{
		*logdetail = "user verification required but not performed";
		return false;
	}

	/* Validate sign counter (anti-replay) */
	elog(DEBUG1, "FIDO2: sign_count check: received=%u, stored=" INT64_FORMAT,
		 parsed_auth_data.sign_count, matched_cred->sign_count);
	if (parsed_auth_data.sign_count > 0 &&
		matched_cred->sign_count > 0 &&
		parsed_auth_data.sign_count <= (uint32) matched_cred->sign_count)
	{
		*logdetail = "signature counter did not increase (possible cloned authenticator)";
		return false;
	}

	/*
	 * Build the signed data for verification.
	 *
	 * In WebAuthn, the signature is over: authenticatorData || SHA256(clientDataJSON)
	 *
	 * For our simplified SASL protocol, clientDataJSON is constructed from:
	 *   - challenge
	 *   - origin (rp_id)
	 *   - type ("webauthn.get")
	 *
	 * We'll compute the hash of this data.
	 */

	/*
	 * Compute client data hash.
	 * For simplicity, we hash: challenge || rp_id_hash
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx)
	{
		*logdetail = "out of memory";
		return false;
	}
	if (pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, state->challenge, FIDO2_CHALLENGE_LENGTH) < 0 ||
		pg_cryptohash_update(sha256_ctx, parsed_auth_data.rp_id_hash,
							 FIDO2_RP_ID_HASH_LENGTH) < 0 ||
		pg_cryptohash_final(sha256_ctx, client_data_hash, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		pg_cryptohash_free(sha256_ctx);
		*logdetail = "SHA-256 computation failed";
		return false;
	}
	pg_cryptohash_free(sha256_ctx);

	/*
	 * Now compute the hash of: authenticatorData || clientDataHash
	 */
	sha256_ctx = pg_cryptohash_create(PG_SHA256);
	if (!sha256_ctx)
	{
		*logdetail = "out of memory";
		return false;
	}
	if (pg_cryptohash_init(sha256_ctx) < 0 ||
		pg_cryptohash_update(sha256_ctx, auth_data, auth_data_len) < 0 ||
		pg_cryptohash_update(sha256_ctx, client_data_hash, PG_SHA256_DIGEST_LENGTH) < 0 ||
		pg_cryptohash_final(sha256_ctx, signed_data_hash, PG_SHA256_DIGEST_LENGTH) < 0)
	{
		pg_cryptohash_free(sha256_ctx);
		*logdetail = "SHA-256 computation failed";
		return false;
	}
	pg_cryptohash_free(sha256_ctx);

	/*
	 * Verify the signature using dual verification.
	 * The public key is stored as uncompressed EC point (65 bytes: 0x04 || X || Y)
	 */
	if (matched_cred->public_key_len != 65)
	{
		*logdetail = "invalid public key length";
		return false;
	}

	elog(DEBUG1, "FIDO2: verifying ES256 signature (public_key_len=%d)",
		 matched_cred->public_key_len);
	verify_result = fido2_verify_es256_raw(matched_cred->public_key,
										   signed_data_hash,
										   signature);

	if (verify_result == FIDO2_VERIFY_DISAGREE)
	{
		elog(WARNING, "FIDO2 verification implementations disagree - possible attack or bug");
		*logdetail = "signature verification error";
		return false;
	}

	if (verify_result != FIDO2_VERIFY_OK)
	{
		*logdetail = "signature verification failed";
		return false;
	}

	elog(DEBUG1, "FIDO2: signature verification successful");

	/* Update sign counter */
	if (parsed_auth_data.sign_count > 0)
		update_sign_count(matched_cred->oid, parsed_auth_data.sign_count);

	return true;
}

/*
 * Update the sign_count in pg_role_pubkeys after successful authentication
 */
static void
update_sign_count(Oid credential_oid, int64 new_count)
{
	/* TODO: Update the sign_count in pg_role_pubkeys */
	/* This requires appropriate permissions and catalog update code */
}
