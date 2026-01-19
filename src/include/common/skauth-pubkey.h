/*-------------------------------------------------------------------------
 *
 * skauth-pubkey.h
 *	  OpenSSH sk-ecdsa public key parser for sk-provider authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/skauth-pubkey.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKAUTH_PUBKEY_H
#define SKAUTH_PUBKEY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* COSE algorithm identifier for ES256 */
#define SKAUTH_ALG_ES256		(-7)

/*
 * Parsed public key from OpenSSH sk-ecdsa format
 */
typedef struct SkauthParsedPubkey
{
	uint8_t	   *public_key;		/* EC point (65 bytes for P-256 uncompressed) */
	size_t		public_key_len;
	char	   *application;	/* Relying party ID / application string */
	int			algorithm;		/* COSE algorithm identifier (e.g., -7 for ES256) */
} SkauthParsedPubkey;

/*
 * Parse an OpenSSH sk-ecdsa public key string
 *
 * Input format: "sk-ecdsa-sha2-nistp256@openssh.com AAAA... [comment]"
 *
 * On success, fills in the SkauthParsedPubkey structure and returns true.
 * On failure, sets *errmsg and returns false.
 *
 * The caller must call skauth_free_parsed_pubkey() to free allocated fields.
 */
extern bool skauth_parse_openssh_pubkey(const char *pubkey_str,
										SkauthParsedPubkey *result,
										char **errmsg);

/*
 * Free a parsed public key structure
 */
extern void skauth_free_parsed_pubkey(SkauthParsedPubkey *pubkey);

#endif							/* SKAUTH_PUBKEY_H */
