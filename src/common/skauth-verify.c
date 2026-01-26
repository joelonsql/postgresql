/*-------------------------------------------------------------------------
 * skauth-verify.c
 *	  ECDSA signature verification for ssh-sk authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/skauth-verify.c
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "libpq/skauth.h"

#ifdef USE_OPENSSL

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

SkauthVerifyResult
skauth_verify_es256_raw(const uint8_t *pubkey, const uint8_t *hash, const uint8_t *sig)
{
	EC_KEY	   *key = NULL;
	BIGNUM	   *x = NULL, *y = NULL, *r = NULL, *s = NULL;
	ECDSA_SIG  *esig = NULL;
	SkauthVerifyResult result = SKAUTH_VERIFY_FAIL;

	if (pubkey[0] != 0x04)
		return SKAUTH_VERIFY_FAIL;

	key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!key)
		goto done;

	x = BN_bin2bn(pubkey + 1, 32, NULL);
	y = BN_bin2bn(pubkey + 33, 32, NULL);
	if (!x || !y || EC_KEY_set_public_key_affine_coordinates(key, x, y) != 1)
		goto done;

	esig = ECDSA_SIG_new();
	r = BN_bin2bn(sig, 32, NULL);
	s = BN_bin2bn(sig + 32, 32, NULL);
	if (!esig || !r || !s || ECDSA_SIG_set0(esig, r, s) != 1)
		goto done;
	r = s = NULL;

	if (ECDSA_do_verify(hash, 32, esig, key) == 1)
		result = SKAUTH_VERIFY_OK;

done:
	ECDSA_SIG_free(esig);
	BN_free(r);
	BN_free(s);
	BN_free(x);
	BN_free(y);
	EC_KEY_free(key);
	return result;
}

#else

SkauthVerifyResult
skauth_verify_es256_raw(const uint8_t *pubkey, const uint8_t *hash, const uint8_t *sig)
{
	return SKAUTH_VERIFY_FAIL;
}

#endif
