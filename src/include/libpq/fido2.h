/*-------------------------------------------------------------------------
 * fido2.h
 *	  Definitions for FIDO2 authentication (client and server)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/fido2.h
 *-------------------------------------------------------------------------
 */
#ifndef FIDO2_H
#define FIDO2_H

#include "c.h"
#include <stdint.h>
#include <stddef.h>

/* SASL mechanism name */
#define FIDO2_MECHANISM_NAME	"FIDO2"

/* Hardcoded RP ID for SSH security keys */
#define FIDO2_RP_ID			"ssh:"

/* Protocol version */
#define FIDO2_PROTOCOL_VERSION	1

/* Size constants */
#define FIDO2_CHALLENGE_LENGTH		32
#define FIDO2_ES256_PUBKEY_LENGTH	65
#define FIDO2_ES256_SIG_LENGTH		64

/* Authenticator data flags */
#define FIDO2_FLAG_UP		0x01
#define FIDO2_FLAG_UV		0x04

/* Challenge options flags */
#define FIDO2_OPT_REQUIRE_UP	0x01
#define FIDO2_OPT_REQUIRE_UV	0x02

/* Maximum message sizes */
#define FIDO2_MAX_CHALLENGE_MSG	4096
#define FIDO2_MAX_ASSERTION_MSG	4096

/* COSE algorithm identifier for ES256 */
#define COSE_ALG_ES256		(-7)

/* Verification result codes */
typedef enum Fido2VerifyResult
{
	FIDO2_VERIFY_OK = 0,
	FIDO2_VERIFY_FAIL = 1
} Fido2VerifyResult;

/* Parsed public key from OpenSSH sk-ecdsa format */
typedef struct Fido2ParsedPubkey
{
	uint8_t	   *public_key;
	size_t		public_key_len;
	char	   *application;
	int			algorithm;
} Fido2ParsedPubkey;

/* Parse OpenSSH sk-ecdsa public key string */
extern bool fido2_parse_openssh_pubkey(const char *pubkey_str,
										Fido2ParsedPubkey *result,
										char **errmsg);
extern void fido2_free_parsed_pubkey(Fido2ParsedPubkey *pubkey);

/*
 * X.509 certificate functions for FIDO2 TLS authentication.
 * These are only available when USE_OPENSSL is defined.
 */
#ifdef USE_OPENSSL
#include <openssl/x509.h>
#include <openssl/evp.h>

/* Create EC public key from raw 65-byte uncompressed point */
extern EVP_PKEY *fido2_x509_create_ec_pkey(const uint8_t *pubkey_raw);

/* Build X.509 certificate with FIDO2 assertion extension */
extern bool fido2_x509_build_cert(const uint8_t *pubkey_raw,
								  uint8_t flags, uint32_t counter,
								  const uint8_t *signature,
								  const uint8_t *challenge,
								  X509 **cert_out, EVP_PKEY **pkey_out);

/* Parse FIDO2 assertion from X.509 certificate extension */
extern bool fido2_x509_parse_assertion(X509 *cert,
									   uint8_t *flags, uint32_t *counter,
									   uint8_t *signature, uint8_t *challenge,
									   uint8_t *pubkey);

/* Derive challenge from server's CertificateVerify signature */
extern void fido2_x509_derive_challenge(const uint8_t *server_cv, int server_cv_len,
										uint8_t *challenge);
#endif							/* USE_OPENSSL */

#endif							/* FIDO2_H */
