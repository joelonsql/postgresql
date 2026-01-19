/*-------------------------------------------------------------------------
 *
 * fido2-pubkey.h
 *	  OpenSSH sk-ecdsa public key parser for FIDO2 authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/fido2-pubkey.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FIDO2_PUBKEY_H
#define FIDO2_PUBKEY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* COSE algorithm identifier for ES256 */
#define FIDO2_ALG_ES256		(-7)

/*
 * Parsed public key from OpenSSH sk-ecdsa format
 */
typedef struct Fido2ParsedPubkey
{
	uint8_t	   *public_key;		/* EC point (65 bytes for P-256 uncompressed) */
	size_t		public_key_len;
	char	   *application;	/* Relying party ID / application string */
	int			algorithm;		/* COSE algorithm identifier (e.g., -7 for ES256) */
} Fido2ParsedPubkey;

/*
 * Parse an OpenSSH sk-ecdsa public key string
 *
 * Input format: "sk-ecdsa-sha2-nistp256@openssh.com AAAA... [comment]"
 *
 * On success, fills in the Fido2ParsedPubkey structure and returns true.
 * On failure, sets *errmsg and returns false.
 *
 * The caller must call fido2_free_parsed_pubkey() to free allocated fields.
 */
extern bool fido2_parse_openssh_pubkey(const char *pubkey_str,
									   Fido2ParsedPubkey *result,
									   char **errmsg);

/*
 * Free a parsed public key structure
 */
extern void fido2_free_parsed_pubkey(Fido2ParsedPubkey *pubkey);

#endif							/* FIDO2_PUBKEY_H */
