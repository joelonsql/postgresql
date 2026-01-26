/*-------------------------------------------------------------------------
 * skauth.h
 *	  Definitions for skauth authentication (client and server)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/skauth.h
 *-------------------------------------------------------------------------
 */
#ifndef SKAUTH_H
#define SKAUTH_H

#include "c.h"
#include <stdint.h>
#include <stddef.h>

/* SASL mechanism name */
#define SKAUTH_MECHANISM_NAME	"SKAUTH"

/* Hardcoded RP ID for SSH security keys */
#define SKAUTH_RP_ID			"ssh:"

/* Protocol version */
#define SKAUTH_PROTOCOL_VERSION	1

/* Size constants */
#define SKAUTH_CHALLENGE_LENGTH		32
#define SKAUTH_ES256_PUBKEY_LENGTH	65
#define SKAUTH_ES256_SIG_LENGTH		64

/* Authenticator data flags */
#define SKAUTH_FLAG_UP		0x01
#define SKAUTH_FLAG_UV		0x04

/* Challenge options flags */
#define SKAUTH_OPT_REQUIRE_UP	0x01
#define SKAUTH_OPT_REQUIRE_UV	0x02

/* Maximum message sizes */
#define SKAUTH_MAX_CHALLENGE_MSG	4096
#define SKAUTH_MAX_ASSERTION_MSG	4096

/* COSE algorithm identifier for ES256 */
#define COSE_ALG_ES256		(-7)

/* Verification result codes */
typedef enum SkauthVerifyResult
{
	SKAUTH_VERIFY_OK = 0,
	SKAUTH_VERIFY_FAIL = 1
} SkauthVerifyResult;

/* Parsed public key from OpenSSH sk-ecdsa format */
typedef struct SkauthParsedPubkey
{
	uint8_t	   *public_key;
	size_t		public_key_len;
	char	   *application;
	int			algorithm;
} SkauthParsedPubkey;

/* Parse OpenSSH sk-ecdsa public key string */
extern bool skauth_parse_openssh_pubkey(const char *pubkey_str,
										SkauthParsedPubkey *result,
										char **errmsg);
extern void skauth_free_parsed_pubkey(SkauthParsedPubkey *pubkey);

#endif							/* SKAUTH_H */
