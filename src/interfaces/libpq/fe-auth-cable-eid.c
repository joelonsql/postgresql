/*-------------------------------------------------------------------------
 * fe-auth-cable-eid.c
 *	  caBLE EID (Encrypted IDentifier) decryption for BLE advertisements
 *
 * The EID is broadcast by the phone via BLE after connecting to the tunnel
 * server. It contains the routing_id needed to connect to the correct tunnel.
 *
 * EID Structure (20 bytes total):
 *   Bytes 0-15:  AES-256-ECB encrypted payload (16 bytes)
 *   Bytes 16-19: HMAC-SHA256 truncated to 4 bytes
 *
 * Encrypted Payload (16 bytes plaintext):
 *   Byte 0:      Reserved (must be 0)
 *   Bytes 1-10:  Nonce (10 bytes, random)
 *   Bytes 11-13: Routing ID (3 bytes)
 *   Bytes 14-15: Tunnel Server Domain ID (2 bytes, little-endian)
 *                0 = cable.ua5v.com (Google)
 *                1 = cable.auth.com (Apple)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable-eid.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#ifdef USE_OPENSSL

#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

#include "libpq/cable.h"

/* EID constants */
#define CABLE_EID_LENGTH			20		/* Total encrypted EID size */
#define CABLE_EID_ENCRYPTED_LENGTH	16		/* AES-256-ECB block */
#define CABLE_EID_HMAC_LENGTH		4		/* Truncated HMAC */
#define CABLE_EID_KEY_LENGTH		64		/* AES key (32) + HMAC key (32) */

/* Key derivation constants (from Chromium caBLE implementation) */
#define CABLE_HKDF_EID_KEY			1		/* kEIDKey info value */

/*
 * Derive EID key from QR secret using HKDF-SHA256.
 *
 * The EID key is 64 bytes:
 *   - First 32 bytes: AES-256 encryption key
 *   - Last 32 bytes: HMAC-SHA256 authentication key
 *
 * Derivation follows Chromium's caBLE implementation:
 *   HKDF-SHA256(IKM=qr_secret, salt=empty, info=uint32_le(kEIDKey))
 * where kEIDKey = 1
 *
 * Returns 0 on success, -1 on error.
 */
int
cable_derive_eid_key(const uint8_t *qr_secret, uint8_t *eid_key)
{
	uint8_t		info[4] = {CABLE_HKDF_EID_KEY, 0, 0, 0};  /* kEIDKey = 1 as LE uint32 */
	uint8_t		zero_salt[32] = {0};
	uint8_t		prk[32];				/* pseudo-random key */
	uint8_t		expand_input[37];		/* info (4) + counter (1) + T(n-1) */
	uint8_t		t_prev[32];
	unsigned int hmac_len = 32;

	/*
	 * HKDF-Extract: PRK = HMAC-SHA256(salt=zeros, IKM=secret)
	 * With zero salt, this is effectively HMAC(zeros, secret)
	 */
	if (!HMAC(EVP_sha256(), zero_salt, 32,
			  qr_secret, CABLE_SECRET_LENGTH,
			  prk, &hmac_len))
		return -1;

	/*
	 * HKDF-Expand: Generate 64 bytes (two blocks)
	 *
	 * T(0) = empty
	 * T(1) = HMAC(PRK, info || 0x01) - first 32 bytes
	 * T(2) = HMAC(PRK, T(1) || info || 0x02) - next 32 bytes
	 */

	/* T(1) = HMAC(PRK, info || 0x01) */
	memcpy(expand_input, info, 4);
	expand_input[4] = 0x01;
	if (!HMAC(EVP_sha256(), prk, 32,
			  expand_input, 5,
			  eid_key, &hmac_len))
		return -1;

	/* T(2) = HMAC(PRK, T(1) || info || 0x02) */
	memcpy(t_prev, eid_key, 32);
	memcpy(expand_input, t_prev, 32);
	memcpy(expand_input + 32, info, 4);
	expand_input[36] = 0x02;
	if (!HMAC(EVP_sha256(), prk, 32,
			  expand_input, 37,
			  eid_key + 32, &hmac_len))
		return -1;

	/* Clear sensitive intermediate data */
	explicit_bzero(prk, sizeof(prk));
	explicit_bzero(expand_input, sizeof(expand_input));
	explicit_bzero(t_prev, sizeof(t_prev));

	return 0;
}

/*
 * Decrypt and validate a caBLE EID advertisement.
 *
 * The advertisement is 20 bytes:
 *   - 16 bytes AES-256-ECB encrypted EID
 *   - 4 bytes truncated HMAC-SHA256
 *
 * The eid_key is 64 bytes (from cable_derive_eid_key):
 *   - First 32 bytes: AES-256 key
 *   - Last 32 bytes: HMAC-SHA256 key
 *
 * On success, extracts:
 *   - routing_id: 3 bytes needed for tunnel connection
 *   - tunnel_domain: 0=Google (cable.ua5v.com), 1=Apple (cable.auth.com)
 *   - advert_plaintext: full 16-byte decrypted EID (needed for PSK derivation)
 *
 * The advert_plaintext parameter can be NULL if caller doesn't need it.
 *
 * Returns 0 on success, -1 on error (HMAC mismatch, invalid data, etc.)
 */
int
cable_eid_decrypt(const uint8_t *advert, const uint8_t *eid_key,
				  uint8_t *routing_id, uint16_t *tunnel_domain,
				  uint8_t *advert_plaintext)
{
	uint8_t		calculated_hmac[32];
	uint8_t		plaintext[16];
	unsigned int hmac_len = 32;
	EVP_CIPHER_CTX *ctx = NULL;
	int			outlen;
	int			ret = -1;

	/*
	 * Step 1: Verify HMAC
	 *
	 * HMAC is computed over the encrypted portion (first 16 bytes)
	 * using the HMAC key (last 32 bytes of eid_key).
	 * Only first 4 bytes of HMAC are transmitted.
	 */
	if (!HMAC(EVP_sha256(), eid_key + 32, 32,
			  advert, CABLE_EID_ENCRYPTED_LENGTH,
			  calculated_hmac, &hmac_len))
		return -1;

	if (CRYPTO_memcmp(calculated_hmac, advert + CABLE_EID_ENCRYPTED_LENGTH,
					  CABLE_EID_HMAC_LENGTH) != 0)
	{
		/* HMAC mismatch - this advertisement is not for us */
		return -1;
	}

	/*
	 * Step 2: Decrypt AES-256-ECB
	 *
	 * The encrypted data is exactly one AES block (16 bytes).
	 * AES key is the first 32 bytes of eid_key.
	 */
	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, eid_key, NULL) != 1)
		goto cleanup;

	/* Disable padding since we have exactly one block */
	EVP_CIPHER_CTX_set_padding(ctx, 0);

	if (EVP_DecryptUpdate(ctx, plaintext, &outlen, advert, CABLE_EID_ENCRYPTED_LENGTH) != 1)
		goto cleanup;

	if (outlen != CABLE_EID_ENCRYPTED_LENGTH)
		goto cleanup;

	/*
	 * Step 3: Validate reserved bits
	 *
	 * Byte 0 must be 0 (reserved for future use).
	 */
	if (plaintext[0] != 0)
		goto cleanup;

	/*
	 * Step 4: Extract routing_id (bytes 11-13)
	 *
	 * This is the value the phone received from the tunnel server's
	 * X-caBLE-Routing-ID header when it connected to /cable/new/.
	 */
	memcpy(routing_id, plaintext + 11, CABLE_ROUTING_ID_LENGTH);

	/*
	 * Step 5: Extract and validate tunnel domain (bytes 14-15)
	 *
	 * Little-endian uint16:
	 *   0 = cable.ua5v.com (Google)
	 *   1 = cable.auth.com (Apple)
	 */
	*tunnel_domain = plaintext[14] | (plaintext[15] << 8);
	if (*tunnel_domain > 1)
	{
		/* Invalid domain ID */
		goto cleanup;
	}

	/*
	 * Step 6: Copy full plaintext for PSK derivation (if requested)
	 *
	 * Per FIDO CTAP 2.3, the PSK is derived from both the QR secret and
	 * the full decrypted advertisement plaintext.
	 */
	if (advert_plaintext != NULL)
		memcpy(advert_plaintext, plaintext, 16);

	ret = 0;

cleanup:
	EVP_CIPHER_CTX_free(ctx);
	explicit_bzero(plaintext, sizeof(plaintext));
	explicit_bzero(calculated_hmac, sizeof(calculated_hmac));

	return ret;
}

#else /* !USE_OPENSSL */

/* Stubs when OpenSSL is not available */

int
cable_derive_eid_key(const uint8_t *qr_secret, uint8_t *eid_key)
{
	return -1;
}

int
cable_eid_decrypt(const uint8_t *advert, const uint8_t *eid_key,
				  uint8_t *routing_id, uint16_t *tunnel_domain,
				  uint8_t *advert_plaintext)
{
	return -1;
}

#endif /* USE_OPENSSL */
