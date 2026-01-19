/*-------------------------------------------------------------------------
 *
 * fido2.h
 *	  Shared FIDO2/WebAuthn protocol definitions for client and server
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/fido2.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FIDO2_H
#define FIDO2_H

#include "c.h"

/*
 * SASL mechanism name
 */
#define FIDO2_MECHANISM_NAME	"FIDO2"

/*
 * Protocol version
 */
#define FIDO2_PROTOCOL_VERSION	1

/*
 * Size constants
 */
#define FIDO2_CHALLENGE_LENGTH		32		/* 256 bits of randomness */
#define FIDO2_RP_ID_HASH_LENGTH		32		/* SHA-256 hash of RP ID */
#define FIDO2_AAGUID_LENGTH			16		/* Authenticator AAGUID */
#define FIDO2_ES256_PUBKEY_LENGTH	65		/* Uncompressed EC point (0x04 || X || Y) */
#define FIDO2_ES256_COORD_LENGTH	32		/* Single coordinate length */
#define FIDO2_ES256_SIG_LENGTH		64		/* R || S, each 32 bytes */
#define FIDO2_MAX_CREDENTIAL_ID		1024	/* Maximum credential ID size */
#define FIDO2_MAX_CREDENTIALS		16		/* Maximum credentials per user */

/*
 * COSE algorithm identifiers
 * See: https://www.iana.org/assignments/cose/cose.xhtml#algorithms
 */
#define COSE_ALG_ES256		(-7)	/* ECDSA w/ SHA-256 on P-256 curve */

/*
 * FIDO2 authenticator data flags
 */
#define FIDO2_FLAG_UP		0x01	/* User Present */
#define FIDO2_FLAG_UV		0x04	/* User Verified */
#define FIDO2_FLAG_BE		0x08	/* Backup Eligibility */
#define FIDO2_FLAG_BS		0x10	/* Backup State */
#define FIDO2_FLAG_AT		0x40	/* Attested credential data included */
#define FIDO2_FLAG_ED		0x80	/* Extension data included */

/*
 * SASL protocol message types
 *
 * The FIDO2 SASL exchange:
 *
 * 1. Client -> Server: SASLInitialResponse
 *    - Mechanism: "FIDO2"
 *    - Optional: preferred credential_id (base64)
 *
 * 2. Server -> Client: AUTH_REQ_SASL_CONT with Fido2Challenge
 *    - protocol_version: uint8
 *    - challenge: 32 bytes
 *    - rp_id: string (null-terminated)
 *    - credential_count: uint8
 *    - For each credential:
 *      - credential_id_len: uint16 (big-endian)
 *      - credential_id: bytes (typically rp_id for resident keys)
 *    - options: uint8 flags
 *
 * 3. Client -> Server: SASLResponse with Fido2Assertion
 *    - credential_id_len: uint16 (big-endian)
 *    - credential_id: bytes
 *    - authenticator_data_len: uint16 (big-endian)
 *    - authenticator_data: bytes
 *    - signature_len: uint16 (big-endian)
 *    - signature: bytes (DER or raw, depending on provider)
 *
 * 4. Server -> Client: AUTH_REQ_OK or AUTH_REQ_SASL_FIN
 *
 * Note: Currently only resident (discoverable) credentials are supported.
 * The client uses rp_id to discover matching credentials stored on the
 * authenticator, rather than using the credential_ids as opaque key handles.
 */

/*
 * Challenge options flags (sent from server to client)
 */
#define FIDO2_OPT_REQUIRE_UP	0x01	/* Require user presence */
#define FIDO2_OPT_REQUIRE_UV	0x02	/* Require user verification */

/*
 * Maximum message sizes
 */
#define FIDO2_MAX_CHALLENGE_MSG		4096	/* Max challenge message size */
#define FIDO2_MAX_ASSERTION_MSG		4096	/* Max assertion message size */

/*
 * Build and parse FIDO2 SASL challenge message
 */
typedef struct Fido2Challenge
{
	uint8_t		protocol_version;
	uint8_t		challenge[FIDO2_CHALLENGE_LENGTH];
	char	   *rp_id;
	int			credential_count;
	struct
	{
		uint8_t	   *credential_id;
		int			credential_id_len;
	}		   *credentials;
	uint8_t		options;
} Fido2Challenge;

/*
 * Build and parse FIDO2 SASL assertion response
 */
typedef struct Fido2Assertion
{
	uint8_t	   *credential_id;
	int			credential_id_len;
	uint8_t	   *authenticator_data;
	int			authenticator_data_len;
	uint8_t	   *signature;
	int			signature_len;
} Fido2Assertion;

/*
 * Serialize a challenge message for sending to client
 * Returns allocated buffer, caller must free
 */
extern char *fido2_build_challenge_message(const Fido2Challenge *chal,
										   int *len);

/*
 * Parse a challenge message received from server
 * Returns true on success, sets error on failure
 */
extern bool fido2_parse_challenge_message(const char *data, int len,
										  Fido2Challenge *chal,
										  const char **error);

/*
 * Serialize an assertion response for sending to server
 * Returns allocated buffer, caller must free
 */
extern char *fido2_build_assertion_message(const Fido2Assertion *assertion,
										   int *len);

/*
 * Parse an assertion response received from client
 * Returns true on success, sets error on failure
 */
extern bool fido2_parse_assertion_message(const char *data, int len,
										  Fido2Assertion *assertion,
										  const char **error);

/*
 * Free resources in Fido2Challenge (but not the struct itself)
 */
extern void fido2_free_challenge(Fido2Challenge *chal);

/*
 * Free resources in Fido2Assertion (but not the struct itself)
 */
extern void fido2_free_assertion(Fido2Assertion *assertion);

#endif							/* FIDO2_H */
