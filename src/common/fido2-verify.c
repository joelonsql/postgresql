/*-------------------------------------------------------------------------
 *
 * fido2-verify.c
 *	  Dual ECDSA signature verification for FIDO2 using micro-ecc and bearssl
 *
 * This module provides defense-in-depth verification by requiring BOTH
 * independent ECDSA implementations to agree on signature validity.
 * This protects against bugs or backdoors in either implementation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/fido2-verify.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "common/fido2-verify.h"
#include "ecdsa-uecc/uECC.h"

/* BearSSL headers */
#include "ecdsa-bearssl/inc/bearssl_ec.h"

#include <string.h>

/*
 * Verify an ES256 signature using micro-ecc
 *
 * public_key: 64 bytes (X || Y coordinates, no 0x04 prefix)
 * hash: 32 bytes (SHA-256 hash of signed data)
 * signature: 64 bytes (R || S in raw format)
 *
 * Returns true if signature is valid
 */
static bool
verify_with_uecc(const uint8_t *public_key,
				 const uint8_t *hash,
				 const uint8_t *signature)
{
	uECC_Curve	curve = uECC_secp256r1();

	/*
	 * uECC_verify expects public key without the 0x04 prefix,
	 * hash and signature in the same format as our input.
	 */
	return uECC_verify(public_key, hash, 32, signature, curve) == 1;
}

/*
 * Verify an ES256 signature using bearssl
 *
 * public_key: 64 bytes (X || Y coordinates, no 0x04 prefix)
 * hash: 32 bytes (SHA-256 hash of signed data)
 * signature: 64 bytes (R || S in raw format)
 *
 * Returns true if signature is valid
 */
static bool
verify_with_bearssl(const uint8_t *public_key,
					const uint8_t *hash,
					const uint8_t *signature)
{
	br_ec_public_key pk;
	uint8_t			pubkey_buf[65];

	/*
	 * BearSSL expects uncompressed point format with 0x04 prefix
	 */
	pubkey_buf[0] = 0x04;
	memcpy(pubkey_buf + 1, public_key, 64);

	pk.curve = BR_EC_secp256r1;
	pk.q = pubkey_buf;
	pk.qlen = 65;

	/*
	 * Use the i31 implementation with p256_m31 for raw format verification
	 */
	return br_ecdsa_i31_vrfy_raw(&br_ec_p256_m31, hash, 32, &pk, signature, 64) == 1;
}

/*
 * fido2_verify_es256
 *
 * Verify an ECDSA P-256 (ES256) signature using dual verification.
 * Both micro-ecc and bearssl must agree that the signature is valid.
 *
 * Parameters:
 *   public_key_x: X coordinate of public key (32 bytes)
 *   public_key_y: Y coordinate of public key (32 bytes)
 *   hash: SHA-256 hash of the signed data (32 bytes)
 *   signature_r: R component of signature (32 bytes)
 *   signature_s: S component of signature (32 bytes)
 *
 * Returns:
 *   FIDO2_VERIFY_OK if BOTH implementations verify successfully
 *   FIDO2_VERIFY_FAIL if either implementation rejects the signature
 *   FIDO2_VERIFY_DISAGREE if implementations disagree (serious error!)
 */
Fido2VerifyResult
fido2_verify_es256(const uint8_t *public_key_x,
				   const uint8_t *public_key_y,
				   const uint8_t *hash,
				   const uint8_t *signature_r,
				   const uint8_t *signature_s)
{
	uint8_t		pubkey[64];
	uint8_t		signature[64];
	bool		uecc_result;
	bool		bearssl_result;

	/* Combine coordinates into single buffer */
	memcpy(pubkey, public_key_x, 32);
	memcpy(pubkey + 32, public_key_y, 32);

	/* Combine signature components */
	memcpy(signature, signature_r, 32);
	memcpy(signature + 32, signature_s, 32);

	/* Verify with both implementations */
	uecc_result = verify_with_uecc(pubkey, hash, signature);
	bearssl_result = verify_with_bearssl(pubkey, hash, signature);

	/* Both must agree for defense in depth */
	if (uecc_result && bearssl_result)
		return FIDO2_VERIFY_OK;
	else if (!uecc_result && !bearssl_result)
		return FIDO2_VERIFY_FAIL;
	else
	{
		/*
		 * This is a serious condition - the two implementations disagree.
		 * This could indicate a bug or attack against one implementation.
		 * We treat disagreement as verification failure but return a
		 * distinct code so it can be logged.
		 */
		return FIDO2_VERIFY_DISAGREE;
	}
}

/*
 * fido2_verify_es256_raw
 *
 * Convenience wrapper that takes raw uncompressed public key (65 bytes with
 * 0x04 prefix) and raw signature (64 bytes, R || S).
 */
Fido2VerifyResult
fido2_verify_es256_raw(const uint8_t *public_key_uncompressed,
					   const uint8_t *hash,
					   const uint8_t *signature_raw)
{
	/* Verify the 0x04 prefix for uncompressed point */
	if (public_key_uncompressed[0] != 0x04)
		return FIDO2_VERIFY_FAIL;

	return fido2_verify_es256(public_key_uncompressed + 1,
							  public_key_uncompressed + 33,
							  hash,
							  signature_raw,
							  signature_raw + 32);
}
