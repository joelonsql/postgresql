/*-------------------------------------------------------------------------
 *
 * fido2-verify.h
 *	  Dual ECDSA signature verification for FIDO2
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/fido2-verify.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FIDO2_VERIFY_H
#define FIDO2_VERIFY_H

#include "c.h"
#include <stdint.h>

/*
 * Verification result codes
 */
typedef enum Fido2VerifyResult
{
	FIDO2_VERIFY_OK = 0,		/* Signature valid (both impls agree) */
	FIDO2_VERIFY_FAIL = 1,		/* Signature invalid (both impls agree) */
	FIDO2_VERIFY_DISAGREE = 2	/* Implementations disagree (error!) */
} Fido2VerifyResult;

/*
 * Verify an ES256 (ECDSA P-256 with SHA-256) signature.
 *
 * Uses dual verification with micro-ecc and bearssl. Both must agree
 * that the signature is valid for FIDO2_VERIFY_OK to be returned.
 *
 * Parameters:
 *   public_key_x: X coordinate of public key (32 bytes)
 *   public_key_y: Y coordinate of public key (32 bytes)
 *   hash: SHA-256 hash of the signed data (32 bytes)
 *   signature_r: R component of signature (32 bytes)
 *   signature_s: S component of signature (32 bytes)
 */
extern Fido2VerifyResult fido2_verify_es256(const uint8_t *public_key_x,
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
extern Fido2VerifyResult fido2_verify_es256_raw(const uint8_t *public_key_uncompressed,
												const uint8_t *hash,
												const uint8_t *signature_raw);

#endif							/* FIDO2_VERIFY_H */
