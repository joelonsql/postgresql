/*-------------------------------------------------------------------------
 * fe-auth-cable-noise.c
 *	  Noise protocol implementation for caBLE
 *
 * This implements the Noise_KNpsk0 pattern used by caBLE for establishing
 * an encrypted channel between the client and phone authenticator.
 *
 * The Noise protocol provides:
 * - Mutual authentication via static keys
 * - Forward secrecy via ephemeral keys
 * - Pre-shared key mixing for additional security
 *
 * References:
 * - Noise Protocol Framework: https://noiseprotocol.org/noise.html
 * - caBLE Noise variant: Chromium source
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable-noise.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#ifdef USE_OPENSSL

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "libpq/cable.h"

/* Noise protocol constants */
#define NOISE_HASH_LEN		32
#define NOISE_KEY_LEN		32
#define NOISE_PROTOCOL_NAME	"Noise_KNpsk0_P256_AESGCM_SHA256"

/*
 * HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
 */
static int
hkdf_extract(const uint8_t *salt, size_t salt_len,
			 const uint8_t *ikm, size_t ikm_len,
			 uint8_t *prk)
{
	unsigned int prk_len = 32;

	if (!HMAC(EVP_sha256(), salt, salt_len, ikm, ikm_len, prk, &prk_len))
		return -1;
	return 0;
}

/*
 * HKDF-Expand: OKM = HMAC-Hash(PRK, info || counter)
 */
static int
hkdf_expand(const uint8_t *prk, size_t prk_len,
			const uint8_t *info, size_t info_len,
			uint8_t *okm, size_t okm_len)
{
	HMAC_CTX   *ctx;
	uint8_t		t[32];
	uint8_t		counter = 1;
	size_t		offset = 0;
	unsigned int t_len = 0;

	ctx = HMAC_CTX_new();
	if (!ctx)
		return -1;

	while (offset < okm_len)
	{
		if (!HMAC_Init_ex(ctx, prk, prk_len, EVP_sha256(), NULL))
			goto fail;

		if (offset > 0)
		{
			if (!HMAC_Update(ctx, t, t_len))
				goto fail;
		}

		if (info && info_len > 0)
		{
			if (!HMAC_Update(ctx, info, info_len))
				goto fail;
		}

		if (!HMAC_Update(ctx, &counter, 1))
			goto fail;

		if (!HMAC_Final(ctx, t, &t_len))
			goto fail;

		{
			size_t		copy_len = (okm_len - offset < t_len) ? okm_len - offset : t_len;

			memcpy(okm + offset, t, copy_len);
			offset += copy_len;
		}
		counter++;
	}

	HMAC_CTX_free(ctx);
	return 0;

fail:
	HMAC_CTX_free(ctx);
	return -1;
}

/*
 * HKDF-SHA256: Extract and Expand
 */
static int
hkdf_sha256(const uint8_t *salt, size_t salt_len,
			const uint8_t *ikm, size_t ikm_len,
			const uint8_t *info, size_t info_len,
			uint8_t *okm, size_t okm_len)
{
	uint8_t		prk[32];

	if (hkdf_extract(salt, salt_len, ikm, ikm_len, prk) < 0)
		return -1;

	if (hkdf_expand(prk, 32, info, info_len, okm, okm_len) < 0)
		return -1;

	return 0;
}

/*
 * Perform P-256 ECDH key agreement using EC_KEY API.
 * This is compatible with OpenSSL 1.1 and 3.x.
 */
static int
ecdh_p256(const uint8_t *our_private, size_t priv_len,
		  const uint8_t *their_public, size_t pub_len,
		  uint8_t *shared_secret, size_t *secret_len)
{
	EC_KEY	   *our_key = NULL;
	EC_KEY	   *their_key = NULL;
	EC_GROUP   *group = NULL;
	BIGNUM	   *priv_bn = NULL;
	EC_POINT   *pub_point = NULL;
	EC_POINT   *their_point = NULL;
	int			ret = -1;
	int			field_size;

	/* Create our private key */
	group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	if (!group)
		goto cleanup;

	our_key = EC_KEY_new();
	if (!our_key)
		goto cleanup;

	if (EC_KEY_set_group(our_key, group) != 1)
		goto cleanup;

	priv_bn = BN_bin2bn(our_private, priv_len, NULL);
	if (!priv_bn)
		goto cleanup;

	if (EC_KEY_set_private_key(our_key, priv_bn) != 1)
		goto cleanup;

	/* Compute and set our public key from private key */
	pub_point = EC_POINT_new(group);
	if (!pub_point)
		goto cleanup;

	if (EC_POINT_mul(group, pub_point, priv_bn, NULL, NULL, NULL) != 1)
		goto cleanup;

	if (EC_KEY_set_public_key(our_key, pub_point) != 1)
		goto cleanup;

	/* Create their public key */
	their_key = EC_KEY_new();
	if (!their_key)
		goto cleanup;

	if (EC_KEY_set_group(their_key, group) != 1)
		goto cleanup;

	their_point = EC_POINT_new(group);
	if (!their_point)
		goto cleanup;

	if (EC_POINT_oct2point(group, their_point, their_public, pub_len, NULL) != 1)
		goto cleanup;

	if (EC_KEY_set_public_key(their_key, their_point) != 1)
		goto cleanup;

	/* Perform ECDH */
	field_size = EC_GROUP_get_degree(group);
	*secret_len = (field_size + 7) / 8;

	if (ECDH_compute_key(shared_secret, *secret_len,
						 EC_KEY_get0_public_key(their_key),
						 our_key, NULL) <= 0)
		goto cleanup;

	ret = 0;

cleanup:
	EC_KEY_free(our_key);
	EC_KEY_free(their_key);
	EC_GROUP_free(group);
	BN_free(priv_bn);
	EC_POINT_free(pub_point);
	EC_POINT_free(their_point);
	return ret;
}

/*
 * Mix hash: H = SHA256(H || data)
 */
static void
mix_hash(CableNoiseState *state, const uint8_t *data, size_t len)
{
	SHA256_CTX	ctx;

	SHA256_Init(&ctx);
	SHA256_Update(&ctx, state->handshake_hash, NOISE_HASH_LEN);
	SHA256_Update(&ctx, data, len);
	SHA256_Final(state->handshake_hash, &ctx);
}

/*
 * Mix key: derive new chaining_key and handshake encryption key.
 *
 * In Noise protocol, MixKey produces two outputs:
 *   ck, k = HKDF(ck, input_key_material)
 * The first 32 bytes become the new chaining key (ck).
 * The second 32 bytes become the handshake encryption key (k).
 */
static void
mix_key(CableNoiseState *state, const uint8_t *input_key_material, size_t ikm_len)
{
	uint8_t		temp_key[64];

	/* HKDF-Extract and Expand */
	hkdf_sha256(state->chaining_key, NOISE_KEY_LEN,
				input_key_material, ikm_len,
				NULL, 0,
				temp_key, 64);

	memcpy(state->chaining_key, temp_key, 32);
	memcpy(state->handshake_key, temp_key + 32, 32);	/* Store k for EncryptAndHash */
	state->handshake_nonce = 0;							/* Reset nonce after each MixKey */

	explicit_bzero(temp_key, sizeof(temp_key));
}

/*
 * MixKeyAndHash: derive new chaining_key, mix temp_h into hash, set encryption key.
 *
 * Per Noise spec, MixKeyAndHash produces three outputs from HKDF:
 *   ck, temp_h, temp_k = HKDF(ck, input_key_material, 3)
 * - First 32 bytes: new chaining key
 * - Second 32 bytes: temp_h, mixed into handshake hash
 * - Third 32 bytes: temp_k, becomes the new encryption key
 *
 * This is different from MixKey which only produces 2 outputs (ck, k).
 */
static void
mix_key_and_hash(CableNoiseState *state, const uint8_t *input_key_material, size_t ikm_len)
{
	uint8_t		temp[96];	/* 3 x 32 bytes */

	/* HKDF-Extract and Expand to get 96 bytes */
	hkdf_sha256(state->chaining_key, NOISE_KEY_LEN,
				input_key_material, ikm_len,
				NULL, 0,
				temp, 96);

	/* First 32 bytes: new chaining key */
	memcpy(state->chaining_key, temp, 32);

	/* Second 32 bytes: mix into hash (NOT the raw IKM!) */
	mix_hash(state, temp + 32, 32);

	/* Third 32 bytes: new encryption key */
	memcpy(state->handshake_key, temp + 64, 32);
	state->handshake_nonce = 0;

	explicit_bzero(temp, sizeof(temp));
}

/*
 * AES-256-GCM encryption with associated data.
 */
static int
aead_encrypt(const uint8_t *key, uint32_t counter,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *plaintext, size_t plaintext_len,
			 uint8_t *ciphertext, size_t *ciphertext_len)
{
	EVP_CIPHER_CTX *ctx;
	uint8_t		nonce[CABLE_GCM_NONCE_LENGTH];
	int			len;
	int			ret = -1;

	/* Build nonce from counter */
	memset(nonce, 0, sizeof(nonce));
	nonce[8] = (counter >> 24) & 0xFF;
	nonce[9] = (counter >> 16) & 0xFF;
	nonce[10] = (counter >> 8) & 0xFF;
	nonce[11] = counter & 0xFF;

	/* Debug: log full nonce */
	fprintf(stderr, "[NOISE] aead_encrypt nonce (counter=%u): ", counter);
	for (int i = 0; i < 12; i++)
		fprintf(stderr, "%02x", nonce[i]);
	fprintf(stderr, "\n");

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto cleanup;

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CABLE_GCM_NONCE_LENGTH, NULL) != 1)
		goto cleanup;

	if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
		goto cleanup;

	/* Add AAD */
	if (aad && aad_len > 0)
	{
		if (EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len) != 1)
			goto cleanup;
	}

	/* Encrypt plaintext */
	if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1)
		goto cleanup;
	*ciphertext_len = len;

	if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1)
		goto cleanup;
	*ciphertext_len += len;

	/* Get auth tag */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CABLE_GCM_TAG_LENGTH,
							ciphertext + *ciphertext_len) != 1)
		goto cleanup;
	*ciphertext_len += CABLE_GCM_TAG_LENGTH;

	ret = 0;

cleanup:
	EVP_CIPHER_CTX_free(ctx);
	return ret;
}

/*
 * AES-256-GCM decryption with associated data.
 */
static int
aead_decrypt(const uint8_t *key, uint32_t counter,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *ciphertext, size_t ciphertext_len,
			 uint8_t *plaintext, size_t *plaintext_len)
{
	EVP_CIPHER_CTX *ctx;
	uint8_t		nonce[CABLE_GCM_NONCE_LENGTH];
	int			len;
	int			ret = -1;
	size_t		data_len;

	if (ciphertext_len < CABLE_GCM_TAG_LENGTH)
		return -1;

	data_len = ciphertext_len - CABLE_GCM_TAG_LENGTH;

	/* Build nonce from counter */
	memset(nonce, 0, sizeof(nonce));
	nonce[8] = (counter >> 24) & 0xFF;
	nonce[9] = (counter >> 16) & 0xFF;
	nonce[10] = (counter >> 8) & 0xFF;
	nonce[11] = counter & 0xFF;

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto cleanup;

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CABLE_GCM_NONCE_LENGTH, NULL) != 1)
		goto cleanup;

	if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
		goto cleanup;

	/* Add AAD */
	if (aad && aad_len > 0)
	{
		if (EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len) != 1)
			goto cleanup;
	}

	/* Decrypt ciphertext (excluding tag) */
	if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, data_len) != 1)
		goto cleanup;
	*plaintext_len = len;

	/* Set expected tag */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CABLE_GCM_TAG_LENGTH,
							(void *) (ciphertext + data_len)) != 1)
		goto cleanup;

	if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1)
		goto cleanup;
	*plaintext_len += len;

	ret = 0;

cleanup:
	EVP_CIPHER_CTX_free(ctx);
	return ret;
}

/*
 * EncryptAndHash: encrypt payload using handshake key, mix ciphertext into hash.
 *
 * In Noise protocol, EncryptAndHash(payload) produces ciphertext with GCM tag appended.
 * Even with an empty payload, this produces a 16-byte authentication tag.
 * The ciphertext (including tag) is then mixed into the handshake hash.
 *
 * Returns 0 on success, -1 on error.
 */
static int
encrypt_and_hash(CableNoiseState *state,
				 const uint8_t *plaintext, size_t plaintext_len,
				 uint8_t *ciphertext, size_t *ciphertext_len)
{
	/* Encrypt using handshake_key, nonce, and handshake_hash as AAD */
	if (aead_encrypt(state->handshake_key, state->handshake_nonce,
					 state->handshake_hash, NOISE_HASH_LEN,
					 plaintext, plaintext_len,
					 ciphertext, ciphertext_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: aead_encrypt failed in encrypt_and_hash\n");
		return -1;
	}

	state->handshake_nonce++;

	/* Mix ciphertext (including tag) into hash */
	mix_hash(state, ciphertext, *ciphertext_len);

	return 0;
}

/*
 * DecryptAndHash: decrypt ciphertext using handshake key, mix ciphertext into hash.
 *
 * In Noise protocol, DecryptAndHash verifies the authentication tag and decrypts.
 * The AAD for decryption is the current handshake_hash. After decryption succeeds,
 * the ciphertext (including tag) is mixed into the handshake hash.
 *
 * Returns 0 on success, -1 on error.
 */
static int
decrypt_and_hash(CableNoiseState *state,
				 const uint8_t *ciphertext, size_t ciphertext_len,
				 uint8_t *plaintext, size_t *plaintext_len)
{
	/* Decrypt using handshake_key, nonce, and current handshake_hash as AAD */
	if (aead_decrypt(state->handshake_key, state->handshake_nonce,
					 state->handshake_hash, NOISE_HASH_LEN,
					 ciphertext, ciphertext_len,
					 plaintext, plaintext_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: aead_decrypt failed in decrypt_and_hash\n");
		return -1;
	}

	state->handshake_nonce++;

	/* Mix ciphertext into hash AFTER successful decryption */
	mix_hash(state, ciphertext, ciphertext_len);

	return 0;
}

/*
 * Initialize Noise protocol state.
 */
int
cable_noise_init(CableNoiseState *state,
				 const uint8_t *local_private,
				 const uint8_t *local_public)
{
	size_t		name_len;

	fprintf(stderr, "[NOISE] Initializing Noise state...\n");
	fprintf(stderr, "[NOISE] Protocol: %s\n", NOISE_PROTOCOL_NAME);

	memset(state, 0, sizeof(CableNoiseState));

	/* Copy local static keys */
	memcpy(state->local_static_private, local_private, CABLE_P256_PRIVKEY_LENGTH);
	memcpy(state->local_static_public, local_public, CABLE_P256_PUBKEY_LENGTH);
	fprintf(stderr, "[NOISE] Static public (first 4): %02x %02x %02x %02x\n",
			state->local_static_public[0], state->local_static_public[1],
			state->local_static_public[2], state->local_static_public[3]);

	/*
	 * Initialize handshake hash per Noise spec (Section 5.2):
	 * If len(protocol_name) <= HASHLEN, pad with zeros.
	 * If len(protocol_name) > HASHLEN, hash it.
	 * Our protocol name is 31 bytes < 32, so we pad.
	 */
	name_len = strlen(NOISE_PROTOCOL_NAME);
	if (name_len <= NOISE_HASH_LEN)
	{
		/* Pad with zeros */
		memset(state->handshake_hash, 0, NOISE_HASH_LEN);
		memcpy(state->handshake_hash, NOISE_PROTOCOL_NAME, name_len);
	}
	else
	{
		/* Hash if too long */
		SHA256_CTX	ctx;

		SHA256_Init(&ctx);
		SHA256_Update(&ctx, NOISE_PROTOCOL_NAME, name_len);
		SHA256_Final(state->handshake_hash, &ctx);
	}

	/* Initialize chaining key */
	memcpy(state->chaining_key, state->handshake_hash, NOISE_HASH_LEN);
	fprintf(stderr, "[NOISE] Initial hash/ck (first 4): %02x %02x %02x %02x\n",
			state->handshake_hash[0], state->handshake_hash[1],
			state->handshake_hash[2], state->handshake_hash[3]);

	/* Generate ephemeral key pair */
	if (cable_generate_keypair(state->local_ephemeral_public,
							   state->local_ephemeral_private) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: Failed to generate ephemeral keypair\n");
		return -1;
	}
	fprintf(stderr, "[NOISE] Ephemeral public (first 4): %02x %02x %02x %02x\n",
			state->local_ephemeral_public[0], state->local_ephemeral_public[1],
			state->local_ephemeral_public[2], state->local_ephemeral_public[3]);
	fprintf(stderr, "[NOISE] Init complete\n");

	return 0;
}

/*
 * Derive session keys from the QR secret and BLE advertisement plaintext.
 *
 * For caBLE's KNpsk0 pattern, the sequence before handshake tokens is:
 *   1. MixHash(prologue) - prologue byte 0x01 for KNpsk0
 *   2. MixHash(initiator_static_public) - pre-message pattern for "K"
 *   3. MixKeyAndHash(psk) - mix PSK into both chaining key and hash
 *
 * The "K" in KNpsk0 means the initiator's static key is known to the responder
 * (the phone knows our public key from the QR code). Both sides must mix this
 * pre-shared public key into the transcript for the handshake hashes to match.
 *
 * The prologue byte distinguishes between Noise patterns:
 *   - 0x00 for NKpsk0 (responder's static known to initiator)
 *   - 0x01 for KNpsk0 (initiator's static known to responder)
 *
 * PSK Derivation (per FIDO CTAP 2.3 Section 11.5.1.2):
 *   The PSK is derived from BOTH the QR secret AND the decrypted BLE
 *   advertisement plaintext. The IKM for HKDF is the concatenation:
 *     IKM = qr_secret (16 bytes) || advert_plaintext (16 bytes)
 *
 *   If advert_plaintext is NULL (e.g., when BLE is not available),
 *   we use only the QR secret as IKM (legacy/fallback behavior).
 */
void
cable_noise_derive_keys(CableNoiseState *state,
						const uint8_t *secret,
						const uint8_t *advert_plaintext)
{
	uint8_t		psk[CABLE_PSK_LENGTH];
	uint8_t		prologue[1] = {0x01};	/* KNpsk0 prologue byte */

	fprintf(stderr, "[NOISE] Deriving PSK from secret...\n");
	fprintf(stderr, "[NOISE] Secret (first 4): %02x %02x %02x %02x\n",
			secret[0], secret[1], secret[2], secret[3]);

	/*
	 * Derive PSK per Chromium caBLE v2 implementation:
	 *   psk = Derive<32>(secret, plaintext_eid, DerivedValueType::kPSK)
	 *
	 * The Chromium Derive function uses HKDF-SHA256:
	 *   HKDF(out, out_len, EVP_sha256(),
	 *        secret.data(), secret.size(),      // IKM
	 *        nonce.data(), nonce.size(),        // salt = plaintext_eid
	 *        &type32, sizeof(type32));          // info = purpose
	 *
	 * So: PSK = HKDF(IKM=secret, salt=advert_plaintext, info=purpose_le32)
	 *
	 * If advert_plaintext is NULL (BLE not used), use empty salt.
	 *
	 * Key derivation constants: kEIDKey=1, kTunnelID=2, kPSK=3
	 */
	{
		uint8_t		info[4] = {3, 0, 0, 0};  /* kPSK = 3 as little-endian uint32 */
		uint8_t		empty_salt[1] = {0};
		const uint8_t *salt;
		size_t		salt_len;

		if (advert_plaintext != NULL)
		{
			salt = advert_plaintext;
			salt_len = 16;
			fprintf(stderr, "[NOISE] PSK derivation: HKDF(IKM=secret, salt=advert[16], info=kPSK)\n");
		}
		else
		{
			salt = empty_salt;
			salt_len = 0;
			fprintf(stderr, "[NOISE] PSK derivation: HKDF(IKM=secret, salt=empty, info=kPSK)\n");
		}

		hkdf_sha256(salt, salt_len,
					secret, CABLE_SECRET_LENGTH,
					info, 4,
					psk, CABLE_PSK_LENGTH);
	}

	fprintf(stderr, "[NOISE] Derived PSK (first 4): %02x %02x %02x %02x\n",
			psk[0], psk[1], psk[2], psk[3]);

	/*
	 * Mix prologue into hash first (before PSK).
	 * For KNpsk0 pattern, prologue is a single byte 0x01.
	 */
	mix_hash(state, prologue, sizeof(prologue));
	fprintf(stderr, "[NOISE] Mixed prologue (0x%02x) into hash\n", prologue[0]);

	/*
	 * Mix our static public key into hash (pre-message pattern).
	 * In KNpsk0, the "K" means our static key is known to the responder (phone).
	 * Both sides must include it in the transcript for hashes to match.
	 *
	 * Per Chromium's MixHashPoint implementation, the UNCOMPRESSED (65 byte)
	 * format is used, not the compressed form from the QR code. The phone
	 * decompresses the key from QR before mixing.
	 */
	mix_hash(state, state->local_static_public, CABLE_P256_PUBKEY_LENGTH);
	fprintf(stderr, "[NOISE] Mixed static public key (65 bytes uncompressed) into hash\n");
	fprintf(stderr, "[NOISE] Key prefix: %02x\n", state->local_static_public[0]);

	/* MixKeyAndHash(psk) - per Noise spec, derives ck/temp_h/temp_k from HKDF */
	mix_key_and_hash(state, psk, CABLE_PSK_LENGTH);
	fprintf(stderr, "[NOISE] PSK mixed via MixKeyAndHash\n");
	fprintf(stderr, "[NOISE] Hash after psk0 (first 4): %02x %02x %02x %02x\n",
			state->handshake_hash[0], state->handshake_hash[1],
			state->handshake_hash[2], state->handshake_hash[3]);

	explicit_bzero(psk, sizeof(psk));
}

/*
 * Start the Noise handshake (send our ephemeral key).
 * Builds the handshake message: e + EncryptAndHash(empty payload)
 *
 * Per the Noise protocol specification, after sending handshake tokens like 'e',
 * we must also call EncryptAndHash on the payload. Even with an empty payload,
 * this produces a 16-byte GCM authentication tag that must be appended.
 *
 * Output message format: [65 bytes ephemeral key][16 bytes GCM tag] = 81 bytes
 *
 * When remote_public is provided, also performs es ECDH (initiator role).
 * When remote_public is NULL, only sends ephemeral (responder role in caBLE,
 * where the phone initiates but we don't know its static key).
 */
int
cable_noise_handshake_start(CableNoiseState *state,
							const uint8_t *psk,
							const uint8_t *remote_public,
							uint8_t *out_message, size_t *out_len)
{
	uint8_t		encrypted[CABLE_GCM_TAG_LENGTH + 16];	/* tag + potential padding */
	size_t		encrypted_len;

	fprintf(stderr, "[NOISE] handshake_start: remote_public=%s\n",
			remote_public ? "provided" : "NULL (responder mode)");
	fprintf(stderr, "[NOISE] Our ephemeral (first 4): %02x %02x %02x %02x\n",
			state->local_ephemeral_public[0], state->local_ephemeral_public[1],
			state->local_ephemeral_public[2], state->local_ephemeral_public[3]);

	/*
	 * Mix in our ephemeral public key.
	 *
	 * IMPORTANT: caBLE's variant of Noise requires BOTH mixHash AND mixKey
	 * on the ephemeral public key bytes. This is specified in FIDO CTAP 2.3
	 * Section 11.5.1.2. Standard Noise only calls mixHash here, but caBLE
	 * also derives encryption key material from the ephemeral.
	 */
	mix_hash(state, state->local_ephemeral_public, CABLE_P256_PUBKEY_LENGTH);
	mix_key(state, state->local_ephemeral_public, CABLE_P256_PUBKEY_LENGTH);
	fprintf(stderr, "[NOISE] Mixed our ephemeral into hash and key\n");

	/* e: send ephemeral public key */
	memcpy(out_message, state->local_ephemeral_public, CABLE_P256_PUBKEY_LENGTH);
	*out_len = CABLE_P256_PUBKEY_LENGTH;

	/* es: ECDH with their static, our ephemeral (only if we know their static key) */
	if (remote_public)
	{
		uint8_t		shared_secret[32];
		size_t		secret_len = 32;

		fprintf(stderr, "[NOISE] Computing es ECDH (initiator mode)...\n");
		memcpy(state->remote_static_public, remote_public, CABLE_P256_PUBKEY_LENGTH);

		if (ecdh_p256(state->local_ephemeral_private, CABLE_P256_PRIVKEY_LENGTH,
					  state->remote_static_public, CABLE_P256_PUBKEY_LENGTH,
					  shared_secret, &secret_len) < 0)
		{
			fprintf(stderr, "[NOISE] ERROR: es ECDH failed\n");
			return -1;
		}

		mix_key(state, shared_secret, secret_len);
		fprintf(stderr, "[NOISE] Mixed es secret into chaining key\n");
		explicit_bzero(shared_secret, sizeof(shared_secret));
	}
	else
	{
		fprintf(stderr, "[NOISE] Skipping es ECDH (responder mode)\n");
	}

	/*
	 * EncryptAndHash(empty payload) - produces 16-byte GCM tag.
	 *
	 * Per Noise protocol, after handshake tokens we must encrypt the payload.
	 * Even with an empty payload, this produces an authentication tag that
	 * gets appended to the message and mixed into the handshake hash.
	 */
	if (encrypt_and_hash(state, NULL, 0, encrypted, &encrypted_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: encrypt_and_hash failed\n");
		return -1;
	}

	memcpy(out_message + *out_len, encrypted, encrypted_len);
	*out_len += encrypted_len;
	fprintf(stderr, "[NOISE] Added EncryptAndHash tag: %zu bytes, total=%zu\n",
			encrypted_len, *out_len);

	fprintf(stderr, "[NOISE] handshake_start complete, out_len=%zu\n", *out_len);
	return 0;
}

/*
 * Process incoming ephemeral from the phone (first part of responder flow).
 * Extracts their ephemeral and performs ee ECDH.
 * Call this after receiving the phone's handshake message.
 *
 * The phone's response is 81 bytes:
 *   - 65 bytes: peer ephemeral public key (uncompressed P-256)
 *   - 16 bytes: encrypted empty payload (GCM tag only)
 *
 * Per Chromium v2_handshake.cc ProcessResponse(), we must:
 *   1. MixHash(peer_ephemeral)
 *   2. MixKey(peer_ephemeral)  <-- Critical! Was missing.
 *   3. ECDH(our_ephemeral, peer_ephemeral) -> ee_secret
 *   4. MixKey(ee_secret)
 *   5. DecryptAndHash(ciphertext) to verify the 16-byte tag
 */
int
cable_noise_process_peer_ephemeral(CableNoiseState *state,
								   const uint8_t *message, size_t len)
{
	uint8_t		shared_secret[32];
	size_t		secret_len = 32;
	size_t		expected_len = CABLE_P256_PUBKEY_LENGTH + CABLE_GCM_TAG_LENGTH;

	fprintf(stderr, "[NOISE] process_peer_ephemeral: received %zu bytes (expected %zu)\n",
			len, expected_len);

	if (len < CABLE_P256_PUBKEY_LENGTH)
	{
		fprintf(stderr, "[NOISE] ERROR: message too short (need %d, got %zu)\n",
				CABLE_P256_PUBKEY_LENGTH, len);
		return -1;
	}

	/* Extract peer's ephemeral public key */
	memcpy(state->remote_ephemeral_public, message, CABLE_P256_PUBKEY_LENGTH);
	fprintf(stderr, "[NOISE] Peer ephemeral (first 4): %02x %02x %02x %02x\n",
			state->remote_ephemeral_public[0], state->remote_ephemeral_public[1],
			state->remote_ephemeral_public[2], state->remote_ephemeral_public[3]);

	/*
	 * Mix in their ephemeral public key - need BOTH per Noise spec.
	 * Per Chromium v2_handshake.cc lines 969-970:
	 *   noise_.MixHash(peer_point_bytes);
	 *   noise_.MixKey(peer_point_bytes);
	 */
	mix_hash(state, state->remote_ephemeral_public, CABLE_P256_PUBKEY_LENGTH);
	mix_key(state, state->remote_ephemeral_public, CABLE_P256_PUBKEY_LENGTH);
	fprintf(stderr, "[NOISE] Mixed peer ephemeral into hash AND key\n");

	/* ee: ECDH with our ephemeral, their ephemeral */
	fprintf(stderr, "[NOISE] Computing ee ECDH...\n");
	if (ecdh_p256(state->local_ephemeral_private, CABLE_P256_PRIVKEY_LENGTH,
				  state->remote_ephemeral_public, CABLE_P256_PUBKEY_LENGTH,
				  shared_secret, &secret_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: ee ECDH failed\n");
		return -1;
	}
	fprintf(stderr, "[NOISE] ee ECDH success, secret_len=%zu\n", secret_len);

	mix_key(state, shared_secret, secret_len);
	fprintf(stderr, "[NOISE] Mixed ee secret into chaining key\n");
	explicit_bzero(shared_secret, sizeof(shared_secret));

	/*
	 * Store the encrypted tag from the phone's response for later processing.
	 * The DecryptAndHash must happen AFTER the se ECDH in complete_handshake().
	 * Per Chromium v2_handshake.cc, the order is:
	 *   1. MixHash(peer_ephemeral)
	 *   2. MixKey(peer_ephemeral)
	 *   3. MixKey(ee_secret)
	 *   4. MixKey(se_secret)  <-- in complete_handshake()
	 *   5. DecryptAndHash(ciphertext)  <-- AFTER se, in complete_handshake()
	 */
	if (len > CABLE_P256_PUBKEY_LENGTH)
	{
		size_t ciphertext_len = len - CABLE_P256_PUBKEY_LENGTH;

		if (ciphertext_len > CABLE_GCM_TAG_LENGTH)
		{
			fprintf(stderr, "[NOISE] ERROR: ciphertext too large (%zu > %d)\n",
					ciphertext_len, CABLE_GCM_TAG_LENGTH);
			return -1;
		}

		memcpy(state->peer_ciphertext, message + CABLE_P256_PUBKEY_LENGTH, ciphertext_len);
		state->peer_ciphertext_len = ciphertext_len;
		fprintf(stderr, "[NOISE] Stored %zu byte ciphertext for later DecryptAndHash\n",
				ciphertext_len);
	}
	else
	{
		state->peer_ciphertext_len = 0;
		fprintf(stderr, "[NOISE] WARNING: No ciphertext in response (legacy mode?)\n");
	}

	return 0;
}

/*
 * Complete handshake after sending our ephemeral (second part of responder flow).
 * Performs se ECDH, verifies peer's auth tag via DecryptAndHash, and derives transport keys.
 * Call this after sending our ephemeral via cable_noise_handshake_start().
 *
 * Per Chromium v2_handshake.cc ProcessResponse(), the order MUST be:
 *   1. MixHash(peer_ephemeral)      - done in process_peer_ephemeral
 *   2. MixKey(peer_ephemeral)       - done in process_peer_ephemeral
 *   3. MixKey(ee_secret)            - done in process_peer_ephemeral
 *   4. MixKey(se_secret)            - done here
 *   5. DecryptAndHash(ciphertext)   - done here, AFTER se!
 *   6. Split() -> transport keys    - done here
 */
int
cable_noise_complete_handshake(CableNoiseState *state)
{
	uint8_t		shared_secret[32];
	size_t		secret_len = 32;
	uint8_t		temp_keys[64];

	fprintf(stderr, "[NOISE] complete_handshake: computing se ECDH...\n");
	fprintf(stderr, "[NOISE] Our static public (first 4): %02x %02x %02x %02x\n",
			state->local_static_public[0], state->local_static_public[1],
			state->local_static_public[2], state->local_static_public[3]);
	fprintf(stderr, "[NOISE] Their ephemeral (first 4): %02x %02x %02x %02x\n",
			state->remote_ephemeral_public[0], state->remote_ephemeral_public[1],
			state->remote_ephemeral_public[2], state->remote_ephemeral_public[3]);

	/* se: ECDH with our static, their ephemeral */
	if (ecdh_p256(state->local_static_private, CABLE_P256_PRIVKEY_LENGTH,
				  state->remote_ephemeral_public, CABLE_P256_PUBKEY_LENGTH,
				  shared_secret, &secret_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: se ECDH failed\n");
		return -1;
	}
	fprintf(stderr, "[NOISE] se ECDH success, secret_len=%zu\n", secret_len);

	mix_key(state, shared_secret, secret_len);
	fprintf(stderr, "[NOISE] Mixed se secret into chaining key\n");
	explicit_bzero(shared_secret, sizeof(shared_secret));

	/*
	 * DecryptAndHash on the peer's ciphertext - MUST happen after se ECDH!
	 * Per Chromium v2_handshake.cc line 984:
	 *   auto plaintext = noise_.DecryptAndHash(ciphertext);
	 *
	 * This verifies the phone knows the PSK (proves identity) and updates
	 * the handshake hash with the ciphertext.
	 */
	if (state->peer_ciphertext_len > 0)
	{
		uint8_t		decrypted[16];
		size_t		decrypted_len;

		fprintf(stderr, "[NOISE] DecryptAndHash on %zu byte ciphertext (after se)\n",
				state->peer_ciphertext_len);

		if (decrypt_and_hash(state, state->peer_ciphertext, state->peer_ciphertext_len,
							 decrypted, &decrypted_len) < 0)
		{
			fprintf(stderr, "[NOISE] ERROR: DecryptAndHash failed on response\n");
			return -1;
		}

		/* Plaintext should be empty (just a GCM tag, no actual payload) */
		if (decrypted_len != 0)
		{
			fprintf(stderr, "[NOISE] ERROR: Response plaintext not empty (%zu bytes)\n",
					decrypted_len);
			return -1;
		}

		fprintf(stderr, "[NOISE] DecryptAndHash succeeded, peer authenticated\n");
	}
	else
	{
		fprintf(stderr, "[NOISE] WARNING: No peer ciphertext to verify\n");
	}

	/* Derive transport keys - Noise Split() with empty info per spec */
	fprintf(stderr, "[NOISE] Deriving transport keys...\n");
	hkdf_sha256(state->chaining_key, NOISE_KEY_LEN,
				NULL, 0,
				NULL, 0,  /* Empty info per Noise protocol */
				temp_keys, 64);

	memcpy(state->send_key, temp_keys, CABLE_SESSION_KEY_LENGTH);
	memcpy(state->recv_key, temp_keys + 32, CABLE_SESSION_KEY_LENGTH);

	fprintf(stderr, "[NOISE] Send key (first 4): %02x %02x %02x %02x\n",
			state->send_key[0], state->send_key[1], state->send_key[2], state->send_key[3]);
	fprintf(stderr, "[NOISE] Recv key (first 4): %02x %02x %02x %02x\n",
			state->recv_key[0], state->recv_key[1], state->recv_key[2], state->recv_key[3]);

	/* Debug: log FULL key material for debugging */
	fprintf(stderr, "[NOISE] FULL send_key: ");
	for (int i = 0; i < 32; i++)
		fprintf(stderr, "%02x", state->send_key[i]);
	fprintf(stderr, "\n[NOISE] FULL recv_key: ");
	for (int i = 0; i < 32; i++)
		fprintf(stderr, "%02x", state->recv_key[i]);
	fprintf(stderr, "\n");

	state->send_counter = 0;
	state->recv_counter = 0;
	state->handshake_complete = true;

	fprintf(stderr, "[NOISE] Handshake complete! Ready for encrypted messages.\n");

	explicit_bzero(temp_keys, sizeof(temp_keys));
	return 0;
}

/*
 * Finish the Noise handshake (legacy initiator role).
 * Processes the response: e, ee, se
 * This combines process_peer_ephemeral and complete_handshake for the
 * initiator case where we've already sent our ephemeral.
 */
int
cable_noise_handshake_finish(CableNoiseState *state,
							 const uint8_t *message, size_t len)
{
	/* Process their ephemeral and do ee ECDH */
	if (cable_noise_process_peer_ephemeral(state, message, len) < 0)
		return -1;

	/* Complete handshake (se ECDH and key derivation) */
	return cable_noise_complete_handshake(state);
}

/*
 * Encrypt a message using the established session key.
 *
 * Per caBLE v2 spec, messages are padded to 32-byte boundaries before encryption.
 * Padding format: [message][zeros][num_zeros as uint8]
 *
 * Per CTAP 2.2 spec: "The additional data for every message is empty."
 */
int
cable_noise_encrypt(CableNoiseState *state,
					const uint8_t *plaintext, size_t plaintext_len,
					uint8_t *ciphertext, size_t *ciphertext_len)
{
	/*
	 * caBLE pads messages to 32-byte boundaries to hide message lengths.
	 * Padding consists of zeros followed by a byte containing the count of zeros.
	 */
	static const size_t kPaddingGranularity = 32;
	size_t		padded_size;
	size_t		num_zeros;
	uint8_t		padded_message[2048];  /* Must be large enough for any message */

	fprintf(stderr, "[NOISE] Encrypting %zu bytes, counter=%u\n",
			plaintext_len, state->send_counter);

	/* Debug: log raw plaintext before padding */
	fprintf(stderr, "[NOISE] Raw plaintext (%zu bytes):\n  ", plaintext_len);
	for (size_t i = 0; i < plaintext_len; i++)
	{
		fprintf(stderr, "%02x", plaintext[i]);
		if ((i + 1) % 32 == 0 && i + 1 < plaintext_len)
			fprintf(stderr, "\n  ");
	}
	fprintf(stderr, "\n");

	if (!state->handshake_complete)
	{
		fprintf(stderr, "[NOISE] ERROR: handshake not complete!\n");
		return -1;
	}

	/* Calculate padded size: round up (len + 1) to multiple of 32 */
	padded_size = ((plaintext_len + 1 + kPaddingGranularity - 1) / kPaddingGranularity) * kPaddingGranularity;
	num_zeros = padded_size - plaintext_len - 1;

	if (padded_size > sizeof(padded_message))
	{
		fprintf(stderr, "[NOISE] ERROR: message too large for padding buffer\n");
		return -1;
	}

	/* Build padded message: [message][zeros][num_zeros] */
	memcpy(padded_message, plaintext, plaintext_len);
	memset(padded_message + plaintext_len, 0, num_zeros);
	padded_message[padded_size - 1] = (uint8_t) num_zeros;

	fprintf(stderr, "[NOISE] Padded to %zu bytes (added %zu zeros)\n",
			padded_size, num_zeros);

	/* Debug: log full padded message */
	fprintf(stderr, "[NOISE] Padded message (%zu bytes):\n  ", padded_size);
	for (size_t i = 0; i < padded_size; i++)
	{
		fprintf(stderr, "%02x", padded_message[i]);
		if ((i + 1) % 32 == 0 && i + 1 < padded_size)
			fprintf(stderr, "\n  ");
	}
	fprintf(stderr, "\n");

	/* Transport messages use empty AAD per spec */
	if (aead_encrypt(state->send_key, state->send_counter,
					 NULL, 0,
					 padded_message, padded_size,
					 ciphertext, ciphertext_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: aead_encrypt failed\n");
		return -1;
	}

	fprintf(stderr, "[NOISE] Encrypted to %zu bytes\n", *ciphertext_len);

	/* Debug: log full ciphertext */
	fprintf(stderr, "[NOISE] Ciphertext (%zu bytes):\n  ", *ciphertext_len);
	for (size_t i = 0; i < *ciphertext_len; i++)
	{
		fprintf(stderr, "%02x", ciphertext[i]);
		if ((i + 1) % 32 == 0 && i + 1 < *ciphertext_len)
			fprintf(stderr, "\n  ");
	}
	fprintf(stderr, "\n");

	state->send_counter++;
	return 0;
}

/*
 * Decrypt a message using the established session key.
 *
 * Per CTAP 2.2 spec: "The additional data for every message is empty."
 */
int
cable_noise_decrypt(CableNoiseState *state,
					const uint8_t *ciphertext, size_t ciphertext_len,
					uint8_t *plaintext, size_t *plaintext_len)
{
	fprintf(stderr, "[NOISE] Decrypting %zu bytes, counter=%u\n",
			ciphertext_len, state->recv_counter);

	if (!state->handshake_complete)
	{
		fprintf(stderr, "[NOISE] ERROR: handshake not complete!\n");
		return -1;
	}

	/* Transport messages use empty AAD per spec */
	if (aead_decrypt(state->recv_key, state->recv_counter,
					 NULL, 0,
					 ciphertext, ciphertext_len,
					 plaintext, plaintext_len) < 0)
	{
		fprintf(stderr, "[NOISE] ERROR: aead_decrypt failed\n");
		return -1;
	}

	fprintf(stderr, "[NOISE] Decrypted to %zu bytes\n", *plaintext_len);
	state->recv_counter++;
	return 0;
}

#else							/* !USE_OPENSSL */

/* Stubs when OpenSSL is not available */

int
cable_noise_init(CableNoiseState *state,
				 const uint8_t *local_private,
				 const uint8_t *local_public)
{
	return -1;
}

void
cable_noise_derive_keys(CableNoiseState *state,
						const uint8_t *secret,
						const uint8_t *advert_plaintext)
{
}

int
cable_noise_handshake_start(CableNoiseState *state,
							const uint8_t *psk,
							const uint8_t *remote_public,
							uint8_t *out_message, size_t *out_len)
{
	return -1;
}

int
cable_noise_handshake_finish(CableNoiseState *state,
							 const uint8_t *message, size_t len)
{
	return -1;
}

int
cable_noise_process_peer_ephemeral(CableNoiseState *state,
								   const uint8_t *message, size_t len)
{
	return -1;
}

int
cable_noise_complete_handshake(CableNoiseState *state)
{
	return -1;
}

int
cable_noise_encrypt(CableNoiseState *state,
					const uint8_t *plaintext, size_t plaintext_len,
					uint8_t *ciphertext, size_t *ciphertext_len)
{
	return -1;
}

int
cable_noise_decrypt(CableNoiseState *state,
					const uint8_t *ciphertext, size_t ciphertext_len,
					uint8_t *plaintext, size_t *plaintext_len)
{
	return -1;
}

#endif							/* USE_OPENSSL */
