/*-------------------------------------------------------------------------
 *
 * skauth.h
 *	  Shared ssh-sk authentication protocol definitions for client and server
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/skauth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKAUTH_H
#define SKAUTH_H

#include "c.h"

#include <stdint.h>

/*
 * SASL mechanism name
 */
#define SKAUTH_MECHANISM_NAME	"ssh-sk"

/*
 * Hardcoded RP ID (application) for SSH security keys
 */
#define SKAUTH_RP_ID			"ssh:"

/*
 * Protocol version
 */
#define SKAUTH_PROTOCOL_VERSION	1

/*
 * Size constants
 */
#define SKAUTH_CHALLENGE_LENGTH		32		/* 256 bits of randomness */
#define SKAUTH_RP_ID_HASH_LENGTH	32		/* SHA-256 hash of RP ID */
#define SKAUTH_ES256_PUBKEY_LENGTH	65		/* Uncompressed EC point (0x04 || X || Y) */
#define SKAUTH_ES256_COORD_LENGTH	32		/* Single coordinate length */
#define SKAUTH_ES256_SIG_LENGTH		64		/* R || S, each 32 bytes */

/*
 * Authenticator data flags
 */
#define SKAUTH_FLAG_UP		0x01	/* User Present */
#define SKAUTH_FLAG_UV		0x04	/* User Verified */

/*
 * SSH-SK SASL Protocol
 *
 * This protocol follows the SSH model: client proposes a public key,
 * server accepts or rejects. If rejected, client can restart SASL
 * with a different key.
 *
 * 1. Client -> Server: client-first
 *    - public_key:   65 bytes (uncompressed EC point: 0x04 || X || Y)
 *
 * 2. Server -> Client: server-challenge (if key is registered)
 *    - version:      1 byte (0x01)
 *    - challenge:    32 bytes
 *    - options:      1 byte (UP/UV requirements)
 *
 *    OR error if key not registered (client can restart with next key)
 *
 * 3. Client -> Server: client-response
 *    - sig_flags:    1 byte (from authenticator)
 *    - counter:      4 bytes (big-endian, needed for signature verification)
 *    - signature:    64 bytes (R || S)
 *
 * 4. Server -> Client: AUTH_REQ_OK or error
 *
 * The client discovers resident keys using the hardcoded RP ID "ssh:".
 * The server reconstructs authenticatorData for signature verification.
 *
 * Note: The signature counter is transmitted for signature verification
 * (it's part of the signed authenticator data) but is not validated or
 * stored. Modern FIDO2 authenticators often return 0 for privacy reasons.
 */

/*
 * Challenge options flags (sent from server to client)
 */
#define SKAUTH_OPT_REQUIRE_UP	0x01	/* Require user presence */
#define SKAUTH_OPT_REQUIRE_UV	0x02	/* Require user verification */

/*
 * Maximum message sizes
 */
#define SKAUTH_MAX_CHALLENGE_MSG	4096	/* Max challenge message size */
#define SKAUTH_MAX_ASSERTION_MSG	4096	/* Max assertion message size */

/*
 * Server challenge message (sent after client proposes a valid public key)
 */
typedef struct SkauthChallenge
{
	uint8_t		protocol_version;
	uint8_t		challenge[SKAUTH_CHALLENGE_LENGTH];
	uint8_t		options;
} SkauthChallenge;

/*
 * Client assertion response
 */
typedef struct SkauthAssertion
{
	uint8_t		sig_flags;			/* Flags from authenticator */
	uint32_t	counter;			/* Signature counter */
	uint8_t		signature[SKAUTH_ES256_SIG_LENGTH];	/* R || S */
} SkauthAssertion;

#endif							/* SKAUTH_H */
