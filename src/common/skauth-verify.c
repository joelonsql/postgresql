/*-------------------------------------------------------------------------
 *
 * skauth-verify.c
 *	  ECDSA signature verification for sk-provider authentication using OpenSSL
 *
 * This module provides ES256 (ECDSA P-256 with SHA-256) signature
 * verification using OpenSSL's cryptographic implementation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/skauth-verify.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "common/skauth-verify.h"

#ifdef USE_OPENSSL

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

#include <string.h>

/*
 * skauth_verify_es256
 *
 * Verify an ECDSA P-256 (ES256) signature using OpenSSL.
 *
 * Parameters:
 *   public_key_x: X coordinate of public key (32 bytes)
 *   public_key_y: Y coordinate of public key (32 bytes)
 *   hash: SHA-256 hash of the signed data (32 bytes)
 *   signature_r: R component of signature (32 bytes)
 *   signature_s: S component of signature (32 bytes)
 *
 * Returns:
 *   SKAUTH_VERIFY_OK if signature verification succeeds
 *   SKAUTH_VERIFY_FAIL if verification fails or on error
 */
SkauthVerifyResult
skauth_verify_es256(const uint8_t *public_key_x,
					const uint8_t *public_key_y,
					const uint8_t *hash,
					const uint8_t *signature_r,
					const uint8_t *signature_s)
{
	EC_KEY	   *ec_key = NULL;
	EC_GROUP   *ec_group = NULL;
	BIGNUM	   *x = NULL;
	BIGNUM	   *y = NULL;
	BIGNUM	   *r = NULL;
	BIGNUM	   *s = NULL;
	ECDSA_SIG  *sig = NULL;
	SkauthVerifyResult result = SKAUTH_VERIFY_FAIL;
	int			verify_result;

	/* Create EC_KEY for P-256 curve */
	ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!ec_key)
		goto cleanup;

	ec_group = (EC_GROUP *) EC_KEY_get0_group(ec_key);
	if (!ec_group)
		goto cleanup;

	/* Load public key coordinates */
	x = BN_bin2bn(public_key_x, 32, NULL);
	y = BN_bin2bn(public_key_y, 32, NULL);
	if (!x || !y)
		goto cleanup;

	/* Set the public key */
	if (EC_KEY_set_public_key_affine_coordinates(ec_key, x, y) != 1)
		goto cleanup;

	/* Create ECDSA signature from R and S components */
	sig = ECDSA_SIG_new();
	if (!sig)
		goto cleanup;

	r = BN_bin2bn(signature_r, 32, NULL);
	s = BN_bin2bn(signature_s, 32, NULL);
	if (!r || !s)
		goto cleanup;

	/* ECDSA_SIG_set0 takes ownership of r and s */
	if (ECDSA_SIG_set0(sig, r, s) != 1)
		goto cleanup;
	r = NULL;	/* Ownership transferred */
	s = NULL;

	/* Verify the signature */
	verify_result = ECDSA_do_verify(hash, 32, sig, ec_key);
	if (verify_result == 1)
		result = SKAUTH_VERIFY_OK;
	/* verify_result == 0 means invalid signature, -1 means error */

cleanup:
	if (sig)
		ECDSA_SIG_free(sig);
	if (r)
		BN_free(r);
	if (s)
		BN_free(s);
	if (x)
		BN_free(x);
	if (y)
		BN_free(y);
	if (ec_key)
		EC_KEY_free(ec_key);

	return result;
}

/*
 * skauth_verify_es256_raw
 *
 * Convenience wrapper that takes raw uncompressed public key (65 bytes with
 * 0x04 prefix) and raw signature (64 bytes, R || S).
 */
SkauthVerifyResult
skauth_verify_es256_raw(const uint8_t *public_key_uncompressed,
						const uint8_t *hash,
						const uint8_t *signature_raw)
{
	/* Verify the 0x04 prefix for uncompressed point */
	if (public_key_uncompressed[0] != 0x04)
		return SKAUTH_VERIFY_FAIL;

	return skauth_verify_es256(public_key_uncompressed + 1,
							   public_key_uncompressed + 33,
							   hash,
							   signature_raw,
							   signature_raw + 32);
}

#else							/* !USE_OPENSSL */

/*
 * Stub implementations when OpenSSL is not available.
 * sk-provider authentication requires SSL support.
 */

SkauthVerifyResult
skauth_verify_es256(const uint8_t *public_key_x,
					const uint8_t *public_key_y,
					const uint8_t *hash,
					const uint8_t *signature_r,
					const uint8_t *signature_s)
{
	return SKAUTH_VERIFY_FAIL;
}

SkauthVerifyResult
skauth_verify_es256_raw(const uint8_t *public_key_uncompressed,
						const uint8_t *hash,
						const uint8_t *signature_raw)
{
	return SKAUTH_VERIFY_FAIL;
}

#endif							/* USE_OPENSSL */
