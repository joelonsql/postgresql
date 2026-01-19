/*-------------------------------------------------------------------------
 *
 * skauth-verify.h
 *	  ECDSA signature verification for sk-provider authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/skauth-verify.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKAUTH_VERIFY_H
#define SKAUTH_VERIFY_H

#include "c.h"
#include <stdint.h>

/*
 * Verification result codes
 */
typedef enum SkauthVerifyResult
{
	SKAUTH_VERIFY_OK = 0,		/* Signature valid */
	SKAUTH_VERIFY_FAIL = 1		/* Signature invalid or error */
} SkauthVerifyResult;

/*
 * Verify an ES256 (ECDSA P-256 with SHA-256) signature.
 *
 * Uses OpenSSL for ECDSA verification. Returns SKAUTH_VERIFY_FAIL
 * when OpenSSL is not available.
 *
 * Parameters:
 *   public_key_x: X coordinate of public key (32 bytes)
 *   public_key_y: Y coordinate of public key (32 bytes)
 *   hash: SHA-256 hash of the signed data (32 bytes)
 *   signature_r: R component of signature (32 bytes)
 *   signature_s: S component of signature (32 bytes)
 */
extern SkauthVerifyResult skauth_verify_es256(const uint8_t *public_key_x,
											  const uint8_t *public_key_y,
											  const uint8_t *hash,
											  const uint8_t *signature_r,
											  const uint8_t *signature_s);

/*
 * Verify an ES256 signature with raw format inputs.
 *
 * Parameters:
 *   public_key_uncompressed: Uncompressed public key (65 bytes: 0x04 || X || Y)
 *   hash: SHA-256 hash of the signed data (32 bytes)
 *   signature_raw: Raw signature (64 bytes: R || S)
 */
extern SkauthVerifyResult skauth_verify_es256_raw(const uint8_t *public_key_uncompressed,
												  const uint8_t *hash,
												  const uint8_t *signature_raw);

#endif							/* SKAUTH_VERIFY_H */
