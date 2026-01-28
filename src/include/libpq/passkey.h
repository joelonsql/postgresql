/*-------------------------------------------------------------------------
 * passkey.h
 *	  Definitions for Passkey authentication (client and server)
 *
 * Passkey authentication uses native platform APIs (macOS AuthenticationServices,
 * Windows Hello, etc.) to perform WebAuthn-compatible authentication with
 * hardware security keys and platform authenticators.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/passkey.h
 *-------------------------------------------------------------------------
 */
#ifndef PASSKEY_H
#define PASSKEY_H

#include <stdint.h>
#include <stddef.h>

/* SASL mechanism name */
#define PASSKEY_MECHANISM_NAME	"PASSKEY"

/* Protocol version */
#define PASSKEY_PROTOCOL_VERSION	1

/* Size constants */
#define PASSKEY_CHALLENGE_LENGTH		32
#define PASSKEY_MAX_RP_ID_LENGTH		253		/* Max DNS hostname length */
#define PASSKEY_ES256_PUBKEY_LENGTH		65		/* Uncompressed P-256 point */
#define PASSKEY_ES256_SIG_MAX_LENGTH	72		/* DER-encoded ECDSA signature */

/* Authenticator data minimum length: rpIdHash(32) + flags(1) + counter(4) */
#define PASSKEY_AUTH_DATA_MIN_LENGTH	37

/* Authenticator data flags */
#define PASSKEY_FLAG_UP		0x01	/* User Present */
#define PASSKEY_FLAG_UV		0x04	/* User Verified */
#define PASSKEY_FLAG_AT		0x40	/* Attested credential data included */
#define PASSKEY_FLAG_ED		0x80	/* Extension data included */

/* Challenge options flags */
#define PASSKEY_OPT_REQUIRE_UP	0x01
#define PASSKEY_OPT_REQUIRE_UV	0x02

/* Protocol message types (first byte of message) */
#define PASSKEY_MSG_PASSWORD_REQUEST	0x01
#define PASSKEY_MSG_PASSWORD_RESPONSE	0x02
#define PASSKEY_MSG_PASSKEY_CHALLENGE	0x03
#define PASSKEY_MSG_PASSKEY_RESPONSE	0x04

/* Passkey operation types */
#define PASSKEY_OP_GET_ASSERTION		0x01
#define PASSKEY_OP_MAKE_CREDENTIAL		0x02

/* Maximum message sizes */
#define PASSKEY_MAX_CHALLENGE_MSG	4096
#define PASSKEY_MAX_RESPONSE_MSG	8192

/* Credential type identifiers */
#define PASSKEY_CRED_TYPE_FIDO2		1	/* FIDO2/sk-api (OpenSSH compatible) */
#define PASSKEY_CRED_TYPE_WEBAUTHN	2	/* WebAuthn/Passkey (native API) */

/* COSE algorithm identifier for ES256 (also defined in pg_role_pubkeys.h) */
#ifndef COSE_ALG_ES256
#define COSE_ALG_ES256		(-7)
#endif

/* Verification result codes */
typedef enum PasskeyVerifyResult
{
	PASSKEY_VERIFY_OK = 0,
	PASSKEY_VERIFY_FAIL = 1
} PasskeyVerifyResult;

/*
 * WebAuthn credential structure for storing passkey credentials.
 * This is used for both registration and lookup.
 */
typedef struct PasskeyCredential
{
	uint8_t	   *credential_id;
	size_t		credential_id_len;
	uint8_t	   *public_key;			/* 65-byte uncompressed EC point */
	size_t		public_key_len;
	char	   *rp_id;				/* Relying Party ID */
	int16_t		algorithm;			/* COSE algorithm identifier */
} PasskeyCredential;

/* Server-side function to store pending passkey credential after database init */
#ifdef USE_OPENSSL
extern void passkey_store_pending_credential(void);
#endif

#endif							/* PASSKEY_H */
