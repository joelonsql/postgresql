/*-------------------------------------------------------------------------
 * fe-auth-cable.c
 *	  caBLE (Cloud-Assisted BLE) hybrid transport for passkey authentication
 *
 * This implements the caBLE protocol for cross-device WebAuthn authentication.
 * Users scan a QR code displayed by psql to authenticate using their phone.
 *
 * Protocol overview:
 * 1. Generate P-256 keypair and secret
 * 2. Display QR code containing FIDO:/ URL with CBOR-encoded handshake data
 * 3. User scans QR with phone, phone connects to tunnel server
 * 4. Perform Noise protocol handshake over WebSocket tunnel
 * 5. Exchange CTAP2 GetAssertion command/response
 * 6. Return assertion for SASL authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#ifdef USE_OPENSSL

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include "libpq/cable.h"
#include "fe-auth-passkey.h"

/* QR code generation using libqrencode */
#ifdef HAVE_LIBQRENCODE
#include <qrencode.h>
#endif

/*
 * Generate a P-256 key pair for caBLE.
 * Returns 0 on success, -1 on error.
 *
 * Uses the EC_KEY API for compatibility with OpenSSL 1.1 and 3.x.
 */
int
cable_generate_keypair(uint8_t *public_key, uint8_t *private_key)
{
	EC_KEY	   *key = NULL;
	const EC_POINT *pub_point = NULL;
	const BIGNUM *priv_bn = NULL;
	const EC_GROUP *group = NULL;
	int			ret = -1;
	size_t		pub_len;

	/* Create new EC key with P-256 curve */
	key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!key)
		goto cleanup;

	/* Generate the key pair */
	if (EC_KEY_generate_key(key) != 1)
		goto cleanup;

	/* Get the group for point encoding */
	group = EC_KEY_get0_group(key);
	if (!group)
		goto cleanup;

	/* Get and encode the public key in uncompressed format */
	pub_point = EC_KEY_get0_public_key(key);
	if (!pub_point)
		goto cleanup;

	pub_len = EC_POINT_point2oct(group, pub_point, POINT_CONVERSION_UNCOMPRESSED,
								 public_key, CABLE_P256_PUBKEY_LENGTH, NULL);
	if (pub_len != CABLE_P256_PUBKEY_LENGTH)
		goto cleanup;

	/* Get and encode the private key */
	priv_bn = EC_KEY_get0_private_key(key);
	if (!priv_bn)
		goto cleanup;

	if (BN_bn2binpad(priv_bn, private_key, CABLE_P256_PRIVKEY_LENGTH) != CABLE_P256_PRIVKEY_LENGTH)
		goto cleanup;

	ret = 0;

cleanup:
	EC_KEY_free(key);
	return ret;
}

/*
 * Generate random bytes for the caBLE secret.
 * Returns 0 on success, -1 on error.
 */
int
cable_generate_secret(uint8_t *secret, size_t len)
{
	if (RAND_bytes(secret, len) != 1)
		return -1;
	return 0;
}

/*
 * Convert CBOR bytes to base10 digits for FIDO:/ URL.
 *
 * This matches Chromium's BytesToDigits encoding (v2_handshake.cc:458-497):
 * - Process 7-byte chunks as little-endian uint64 -> 17 decimal digits each
 * - Remaining 1-6 bytes use variable digit counts: 1->3, 2->5, 3->8, 4->10, 5->13, 6->15
 *
 * The phone's DigitsToBytes() expects this exact encoding to decode the QR data.
 */
static char *
cbor_to_base10(const uint8_t *cbor, size_t cbor_len)
{
	static const int partial_digits[] = {0, 3, 5, 8, 10, 13, 15};
	char	   *result;
	char	   *ptr;
	size_t		result_size;
	size_t		offset = 0;

	/* Calculate result size */
	result_size = (cbor_len / 7) * 17;
	if (cbor_len % 7 > 0)
		result_size += partial_digits[cbor_len % 7];
	result_size += 1;			/* null terminator */

	result = malloc(result_size);
	if (!result)
		return NULL;
	ptr = result;

	/* Process 7-byte chunks as little-endian uint64 */
	while (cbor_len - offset >= 7)
	{
		uint64_t	v = 0;

		memcpy(&v, cbor + offset, 7);	/* Little-endian on x86/ARM */
		ptr += sprintf(ptr, "%017" PRIu64, v);
		offset += 7;
	}

	/* Process remaining bytes (1-6) with variable digit count */
	if (cbor_len - offset > 0)
	{
		uint64_t	v = 0;
		size_t		remaining = cbor_len - offset;

		memcpy(&v, cbor + offset, remaining);
		ptr += sprintf(ptr, "%0*" PRIu64, partial_digits[remaining], v);
	}

	return result;
}

/*
 * Generate the FIDO:/ URL for QR code display.
 *
 * Format: FIDO:/<base10_digits>
 * The digits are the decimal representation of the CBOR-encoded handshake.
 */
char *
cable_generate_qr_url(const CableQRData *data)
{
	uint8_t	   *cbor;
	size_t		cbor_len;
	char	   *base10;
	char	   *url;
	size_t		url_len;

	/* Encode handshake data to CBOR */
	cbor = cable_cbor_encode_handshake(data, &cbor_len);
	if (!cbor)
		return NULL;

	/* Convert to base10 */
	base10 = cbor_to_base10(cbor, cbor_len);
	free(cbor);

	if (!base10)
		return NULL;

	/* Build FIDO:/ URL */
	url_len = 6 + strlen(base10) + 1;	/* "FIDO:/" + digits + null */
	url = malloc(url_len);
	if (!url)
	{
		free(base10);
		return NULL;
	}

	snprintf(url, url_len, "FIDO:/%s", base10);
	free(base10);

	return url;
}

/*
 * Display a QR code as ASCII art in the terminal using libqrencode.
 */
void
cable_display_qr(const char *fido_url)
{
#ifdef HAVE_LIBQRENCODE
	QRcode	   *qrcode;
	int			row, col;
	int			quiet_zone = 2;
	int			size;

	fprintf(stderr, "\nScan this QR code with your phone to authenticate:\n\n");

	/* Generate QR code with medium error correction */
	qrcode = QRcode_encodeString(fido_url, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
	if (!qrcode)
	{
		fprintf(stderr, "  [QR code generation failed]\n");
		fprintf(stderr, "  Manual URL: %s\n", fido_url);
		return;
	}

	size = qrcode->width;

	/* Top quiet zone */
	for (row = 0; row < quiet_zone; row++)
	{
		fprintf(stderr, "  ");
		for (col = 0; col < size + quiet_zone * 2; col++)
			fprintf(stderr, " ");
		fprintf(stderr, "\n");
	}

	/* QR code rows (2 at a time for compact display) */
	for (row = 0; row < size; row += 2)
	{
		fprintf(stderr, "  ");

		/* Left quiet zone */
		for (col = 0; col < quiet_zone; col++)
			fprintf(stderr, " ");

		for (col = 0; col < size; col++)
		{
			int			top = (qrcode->data[row * size + col] & 1);
			int			bottom = (row + 1 < size) ? (qrcode->data[(row + 1) * size + col] & 1) : 0;

			if (top && bottom)
				fprintf(stderr, "\342\226\210");		/* Full block █ */
			else if (top)
				fprintf(stderr, "\342\226\200");		/* Upper half ▀ */
			else if (bottom)
				fprintf(stderr, "\342\226\204");		/* Lower half ▄ */
			else
				fprintf(stderr, " ");
		}

		/* Right quiet zone */
		for (col = 0; col < quiet_zone; col++)
			fprintf(stderr, " ");

		fprintf(stderr, "\n");
	}

	/* Bottom quiet zone */
	for (row = 0; row < quiet_zone; row++)
	{
		fprintf(stderr, "  ");
		for (col = 0; col < size + quiet_zone * 2; col++)
			fprintf(stderr, " ");
		fprintf(stderr, "\n");
	}

	QRcode_free(qrcode);
#else
	/* Fallback when libqrencode is not available */
	fprintf(stderr, "\nQR code display not available (libqrencode not installed).\n");
	fprintf(stderr, "Install libqrencode for QR code support.\n\n");
	fprintf(stderr, "Manual URL: %s\n", fido_url);
#endif

	fprintf(stderr, "\nWaiting for passkey authentication...\n");
	fprintf(stderr, "(Press Ctrl+C to cancel)\n\n");
}


/*
 * Full caBLE authentication flow.
 *
 * This function orchestrates the complete caBLE handshake:
 * 1. Generate keypair and secret
 * 2. Display QR code
 * 3. Connect to tunnel servers (try both Google and Apple)
 * 4. Wait for phone to connect
 * 5. Perform Noise handshake
 * 6. Exchange CTAP2 GetAssertion
 * 7. Return assertion result
 *
 * credential_id/credential_id_len: Optional credential ID to include in
 * allowCredentials. Required for non-discoverable credentials.
 */
PasskeyAssertion *
cable_get_assertion(const char *rp_id,
					const uint8_t *challenge,
					size_t challenge_len,
					const uint8_t *credential_id,
					size_t credential_id_len)
{
	CableQRData qr_data;
	CableTunnel *tunnel = NULL;
	PasskeyAssertion *assertion = NULL;
	char	   *fido_url = NULL;
	uint8_t		private_key[CABLE_P256_PRIVKEY_LENGTH];
	uint8_t		client_data_hash[32];
	uint8_t	   *ctap_request = NULL;
	size_t		ctap_request_len;
	uint8_t	   *response = NULL;
	size_t		response_len;
	char		client_data_json[512];
	int			json_len;
	uint8_t		advert_plaintext[16];
	bool		have_advert_plaintext = false;

	assertion = calloc(1, sizeof(PasskeyAssertion));
	if (!assertion)
		return NULL;

	/* Generate P-256 key pair */
	if (cable_generate_keypair(qr_data.peer_identity, private_key) < 0)
	{
		assertion->error_message = strdup("failed to generate key pair");
		goto cleanup;
	}

	/* Generate random secret */
	if (cable_generate_secret(qr_data.secret, CABLE_SECRET_LENGTH) < 0)
	{
		assertion->error_message = strdup("failed to generate secret");
		goto cleanup;
	}

	/* Set QR data fields */
	qr_data.known_domains = 2;	/* Number of known tunnel domains (2 = Google + Apple) */
	qr_data.request_type = CABLE_REQUEST_TYPE_GET_ASSERTION;

	/* Generate FIDO:/ URL and display QR code */
	fido_url = cable_generate_qr_url(&qr_data);
	if (!fido_url)
	{
		assertion->error_message = strdup("failed to generate QR code URL");
		goto cleanup;
	}

	fprintf(stderr, "[DEBUG] Generated FIDO URL: %s\n", fido_url);
	fprintf(stderr, "[DEBUG] URL length: %zu\n", strlen(fido_url));

	cable_display_qr(fido_url);

	/* Create tunnel and connect to server */
	tunnel = cable_tunnel_new();
	if (!tunnel)
	{
		assertion->error_message = strdup("failed to create tunnel");
		goto cleanup;
	}

	/*
	 * Derive tunnel ID from secret using HKDF.
	 *
	 * Per Chromium's caBLE implementation:
	 *   HKDF-SHA256(IKM=secret, salt=empty, info=type_value)
	 * where type_value is a 4-byte uint32:
	 *   kTunnelID = 2
	 *   kEIDKey = 1
	 *
	 * The routing ID comes from the phone via BLE advertisement.
	 * The phone gets it from the tunnel server's X-caBLE-Routing-ID header
	 * and broadcasts it in an encrypted EID that we decrypt.
	 */
	{
		uint8_t		tunnel_id[CABLE_TUNNEL_ID_LENGTH];
		uint8_t		routing_id[CABLE_ROUTING_ID_LENGTH];
		uint8_t		eid_key[64];
		uint16_t	tunnel_domain;
		const char *tunnel_server;

		fprintf(stderr, "[DEBUG] QR secret (hex): ");
		for (int i = 0; i < CABLE_SECRET_LENGTH; i++)
			fprintf(stderr, "%02x", qr_data.secret[i]);
		fprintf(stderr, "\n");

		fprintf(stderr, "[DEBUG] Public key (first 8 bytes): ");
		for (int i = 0; i < 8; i++)
			fprintf(stderr, "%02x", qr_data.peer_identity[i]);
		fprintf(stderr, "...\n");

		/* Initialize advert_plaintext */
		memset(advert_plaintext, 0, sizeof(advert_plaintext));

		/*
		 * Derive tunnel_id using HKDF-SHA256.
		 * Matches Chromium: HKDF(secret, salt=NULL, info=kTunnelID)
		 * kTunnelID = 2 as uint32_t (little-endian: 02 00 00 00)
		 *
		 * HKDF consists of Extract and Expand phases:
		 * Extract: PRK = HMAC(salt, IKM) - with zero salt
		 * Expand: T(1) = HMAC(PRK, info || 0x01)
		 */
		{
			uint8_t		info[4] = {2, 0, 0, 0};  /* kTunnelID = 2 as little-endian uint32 */
			uint8_t		zero_salt[32] = {0};
			uint8_t		prk[32];				/* pseudo-random key */
			uint8_t		expand_input[37];		/* info (4) + counter (1) + overhead */
			unsigned int prk_len = 32;
			unsigned int expand_len = 32;

			/* HKDF-Extract: PRK = HMAC-SHA256(salt=zeros, IKM=secret) */
			HMAC(EVP_sha256(), zero_salt, 32,
				 qr_data.secret, CABLE_SECRET_LENGTH,
				 prk, &prk_len);

			/* HKDF-Expand: T(1) = HMAC-SHA256(PRK, info || 0x01) */
			memcpy(expand_input, info, 4);
			expand_input[4] = 0x01;
			HMAC(EVP_sha256(), prk, 32,
				 expand_input, 5,
				 tunnel_id, &expand_len);

			/* We only need first 16 bytes for tunnel_id */
		}

		fprintf(stderr, "[DEBUG] Derived tunnel_id (hex): ");
		for (int i = 0; i < CABLE_TUNNEL_ID_LENGTH; i++)
			fprintf(stderr, "%02x", tunnel_id[i]);
		fprintf(stderr, "\n");

		/*
		 * Try to get routing_id via BLE if enabled.
		 *
		 * BLE scanning requires Bluetooth entitlements which need a proper
		 * Apple Developer certificate to work. Set PGPASSKEY_BLE=1 to enable
		 * BLE scanning (requires signed binary with Bluetooth entitlement).
		 *
		 * Without BLE, we fall back to deriving routing_id from the public key,
		 * which may not work with Apple's tunnel server but allows testing
		 * the rest of the protocol.
		 */
		if (getenv("PGPASSKEY_BLE") != NULL)
		{
			/*
			 * Derive EID key from QR secret for decrypting BLE advertisements.
			 * The phone will broadcast an encrypted EID containing the routing_id
			 * that the tunnel server assigned to it.
			 */
			if (cable_derive_eid_key(qr_data.secret, eid_key) < 0)
			{
				assertion->error_message = strdup("failed to derive EID key");
				goto cleanup;
			}

			/*
			 * Start BLE scanning for caBLE advertisements.
			 * The phone will broadcast after connecting to the tunnel server.
			 */
			if (cable_ble_start_scan(eid_key, sizeof(eid_key)) < 0)
			{
				fprintf(stderr, "[BLE] Failed to start BLE scanner - falling back to derived routing_id\n");
				fprintf(stderr, "[BLE] Note: BLE requires Bluetooth entitlement with proper code signing\n");
				explicit_bzero(eid_key, sizeof(eid_key));
				goto fallback_routing_id;
			}

			/*
			 * Wait for the phone to broadcast its EID via BLE.
			 * This contains the routing_id assigned by the tunnel server
			 * and the advert_plaintext needed for PSK derivation.
			 */
			fprintf(stderr, "[DEBUG] Waiting for BLE advertisement from phone...\n");
			if (cable_ble_wait_for_advert(routing_id, &tunnel_domain, advert_plaintext,
										  CABLE_AUTH_TIMEOUT_SECS) < 0)
			{
				cable_ble_stop_scan();
				fprintf(stderr, "[BLE] BLE advertisement not received - falling back to derived routing_id\n");
				explicit_bzero(eid_key, sizeof(eid_key));
				goto fallback_routing_id;
			}

			cable_ble_stop_scan();
			explicit_bzero(eid_key, sizeof(eid_key));
			have_advert_plaintext = true;

			fprintf(stderr, "[DEBUG] Received routing_id via BLE (hex): ");
			for (int i = 0; i < CABLE_ROUTING_ID_LENGTH; i++)
				fprintf(stderr, "%02x", routing_id[i]);
			fprintf(stderr, "\n");
			fprintf(stderr, "[DEBUG] Received advert_plaintext (hex): ");
			for (int i = 0; i < 16; i++)
				fprintf(stderr, "%02x", advert_plaintext[i]);
			fprintf(stderr, "\n");

			/* Select tunnel server based on domain ID from EID */
			tunnel_server = (tunnel_domain == CABLE_DOMAIN_ID_APPLE)
				? CABLE_TUNNEL_SERVER_APPLE
				: CABLE_TUNNEL_SERVER_GOOGLE;
		}
		else
		{
fallback_routing_id:
			/*
			 * Fallback: Derive routing_id from the public key.
			 * routing_id = SHA256(public_key)[0:3]
			 *
			 * Note: This does NOT work with Apple's tunnel server, which
			 * validates that routing_id matches what it assigned to the phone.
			 * For Apple devices, BLE is required.
			 *
			 * This fallback exists for:
			 * 1. Testing the protocol without BLE
			 * 2. Potential future compatibility with other tunnel servers
			 */
			{
				uint8_t		hash[32];

				fprintf(stderr, "[DEBUG] BLE disabled - using derived routing_id (may not work with Apple tunnel)\n");
				fprintf(stderr, "[DEBUG] Set PGPASSKEY_BLE=1 to enable BLE (requires proper code signing)\n");

				SHA256(qr_data.peer_identity, sizeof(qr_data.peer_identity), hash);
				memcpy(routing_id, hash, CABLE_ROUTING_ID_LENGTH);
			}

			/* Default to Apple server for iOS devices */
			tunnel_server = CABLE_TUNNEL_SERVER_APPLE;
		}

		fprintf(stderr, "[DEBUG] routing_id (hex): ");
		for (int i = 0; i < CABLE_ROUTING_ID_LENGTH; i++)
			fprintf(stderr, "%02x", routing_id[i]);
		fprintf(stderr, "\n");

		fprintf(stderr, "[DEBUG] Connecting to %s tunnel server...\n", tunnel_server);
		if (cable_tunnel_connect(tunnel, tunnel_server, tunnel_id, routing_id) < 0)
		{
			assertion->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}
	}

	/*
	 * Initialize Noise protocol and perform handshake immediately.
	 * Note: We send first (initiator role), don't wait for peer.
	 */
	if (cable_noise_init(&tunnel->noise, private_key, qr_data.peer_identity) < 0)
	{
		assertion->error_message = strdup("failed to initialize Noise protocol");
		goto cleanup;
	}

	/*
	 * Derive keys for Noise handshake.
	 *
	 * Per FIDO CTAP 2.3, PSK is derived from both the QR secret and the
	 * BLE advertisement plaintext. If we received BLE, pass it; otherwise
	 * pass NULL for fallback (legacy/non-BLE mode).
	 */
	cable_noise_derive_keys(&tunnel->noise, qr_data.secret,
							have_advert_plaintext ? advert_plaintext : NULL);

	/*
	 * Perform Noise handshake.
	 *
	 * In caBLE, the desktop (QR code generator) is the Noise INITIATOR.
	 * The flow is:
	 *   1. We send: e (our ephemeral public key)
	 *   2. Phone receives it, computes es ECDH (using our static from QR)
	 *   3. Phone sends: e (its ephemeral public key)
	 *   4. We receive it, compute ee and se ECDH, derive transport keys
	 *
	 * The order of operations matters for the transcript hash.
	 */
	{
		uint8_t    *phone_msg = NULL;
		size_t		phone_msg_len;
		uint8_t		handshake_msg[CABLE_P256_PUBKEY_LENGTH + CABLE_GCM_TAG_LENGTH];	/* 65 + 16 = 81 */
		size_t		handshake_len;

		fprintf(stderr, "[DEBUG] Starting Noise handshake (initiator role)\n");

		/* Step 1: Send our ephemeral public key */
		fprintf(stderr, "[DEBUG] Generating our ephemeral key...\n");
		if (cable_noise_handshake_start(&tunnel->noise, NULL, NULL,
										handshake_msg, &handshake_len) < 0)
		{
			fprintf(stderr, "[DEBUG] Failed to generate handshake\n");
			assertion->error_message = strdup("failed to generate handshake");
			goto cleanup;
		}

		fprintf(stderr, "[DEBUG] Sending our ephemeral: %zu bytes\n", handshake_len);
		fprintf(stderr, "[DEBUG] Our ephemeral first bytes: %02x %02x %02x %02x ...\n",
				handshake_msg[0], handshake_msg[1], handshake_msg[2], handshake_msg[3]);

		if (cable_tunnel_send(tunnel, handshake_msg, handshake_len) < 0)
		{
			fprintf(stderr, "[DEBUG] Failed to send our ephemeral: %s\n", cable_tunnel_error(tunnel));
			assertion->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}
		fprintf(stderr, "[DEBUG] Sent our ephemeral successfully\n");

		/* Step 2-3: Receive phone's ephemeral and compute ECDH */
		fprintf(stderr, "[DEBUG] Waiting to receive phone's ephemeral key...\n");
		if (cable_tunnel_recv(tunnel, &phone_msg, &phone_msg_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
		{
			fprintf(stderr, "[DEBUG] Failed to receive handshake: %s\n", cable_tunnel_error(tunnel));
			assertion->error_message = strdup("failed to receive handshake from phone");
			goto cleanup;
		}

		fprintf(stderr, "[DEBUG] Received phone message: %zu bytes\n", phone_msg_len);
		if (phone_msg_len > 0)
		{
			fprintf(stderr, "[DEBUG] First bytes: %02x %02x %02x %02x ...\n",
					phone_msg[0],
					phone_msg_len > 1 ? phone_msg[1] : 0,
					phone_msg_len > 2 ? phone_msg[2] : 0,
					phone_msg_len > 3 ? phone_msg[3] : 0);
		}

		fprintf(stderr, "[DEBUG] Processing phone's ephemeral...\n");
		if (cable_noise_process_peer_ephemeral(&tunnel->noise, phone_msg, phone_msg_len) < 0)
		{
			fprintf(stderr, "[DEBUG] Failed to process phone's ephemeral\n");
			assertion->error_message = strdup("failed to process phone's handshake");
			free(phone_msg);
			goto cleanup;
		}
		fprintf(stderr, "[DEBUG] Processed phone's ephemeral successfully\n");
		free(phone_msg);

		/* Step 4: Complete handshake (compute remaining ECDH and derive transport keys) */
		fprintf(stderr, "[DEBUG] Completing handshake (key derivation)...\n");
		if (cable_noise_complete_handshake(&tunnel->noise) < 0)
		{
			fprintf(stderr, "[DEBUG] Failed to complete handshake\n");
			assertion->error_message = strdup("failed to complete handshake");
			goto cleanup;
		}
		fprintf(stderr, "[DEBUG] Noise handshake complete! Ready for encrypted communication.\n");
	}

	/*
	 * Receive and process the post-handshake message from the phone.
	 * Per caBLE v2 protocol, after the Noise handshake the phone sends:
	 *   - Key 1: GetInfo response (authenticator capabilities)
	 *   - Key 2: Linking info (optional)
	 *   - Key 3: Features array (optional, e.g., "ctap", "dc")
	 * We need to consume this message before sending CTAP2 commands.
	 */
	{
		uint8_t    *post_msg = NULL;
		size_t		post_msg_len;
		uint8_t		decrypted[1024];
		size_t		decrypted_len;

		fprintf(stderr, "[DEBUG] Waiting for post-handshake message...\n");

		if (cable_tunnel_recv(tunnel, &post_msg, &post_msg_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
		{
			assertion->error_message = strdup("failed to receive post-handshake message");
			goto cleanup;
		}

		if (cable_noise_decrypt(&tunnel->noise, post_msg, post_msg_len,
								decrypted, &decrypted_len) < 0)
		{
			assertion->error_message = strdup("failed to decrypt post-handshake message");
			free(post_msg);
			goto cleanup;
		}
		free(post_msg);

		fprintf(stderr, "[DEBUG] Received post-handshake message (%zu bytes)\n", decrypted_len);
		fprintf(stderr, "[DEBUG] Post-handshake bytes:");
		for (size_t i = 0; i < decrypted_len && i < 128; i++)
		{
			if (i % 16 == 0)
				fprintf(stderr, "\n  ");
			fprintf(stderr, "%02x ", decrypted[i]);
		}
		fprintf(stderr, "\n");

		/*
		 * Detect protocol revision by checking for padding.
		 * Revision 0: message has null padding at end
		 * Revision 1+: raw CBOR, no padding
		 */
		if (decrypted_len > 0 && decrypted[decrypted_len - 1] > 0 &&
			decrypted[decrypted_len - 1] <= 16)
		{
			uint8_t pad_len = decrypted[decrypted_len - 1];
			bool has_padding = true;

			if (pad_len < decrypted_len)
			{
				for (size_t i = decrypted_len - pad_len; i < decrypted_len - 1; i++)
				{
					if (decrypted[i] != 0)
					{
						has_padding = false;
						break;
					}
				}
			}
			else
			{
				has_padding = false;
			}

			if (has_padding)
			{
				tunnel->noise.protocol_revision = 0;
				fprintf(stderr, "[DEBUG] Detected protocol revision 0 (padded message)\n");
			}
			else
			{
				tunnel->noise.protocol_revision = 1;
				fprintf(stderr, "[DEBUG] Detected protocol revision 1+ (no padding)\n");
			}
		}
		else
		{
			tunnel->noise.protocol_revision = 1;
			fprintf(stderr, "[DEBUG] Assuming protocol revision 1+\n");
		}
	}

	/* Build clientDataJSON */
	json_len = snprintf(client_data_json, sizeof(client_data_json),
						"{\"type\":\"webauthn.get\",\"challenge\":\"%.*s\",\"origin\":\"postgresql://%s\",\"crossOrigin\":false}",
						(int) challenge_len, challenge, rp_id);

	/* Compute clientDataHash */
	{
		SHA256_CTX	sha_ctx;

		SHA256_Init(&sha_ctx);
		SHA256_Update(&sha_ctx, client_data_json, json_len);
		SHA256_Final(client_data_hash, &sha_ctx);
	}

	/* Build CTAP2 GetAssertion request */
	ctap_request = cable_cbor_encode_get_assertion(rp_id, client_data_hash,
												   credential_id, credential_id_len,
												   &ctap_request_len);
	if (!ctap_request)
	{
		assertion->error_message = strdup("failed to encode CTAP2 request");
		goto cleanup;
	}

	/* Encrypt and send request */
	{
		uint8_t		framed_request[1024];
		uint8_t		encrypted[1024];
		size_t		encrypted_len;
		size_t		request_len;

		/*
		 * All CTAP messages are prefixed with a type byte:
		 *   0x00 = Shutdown, 0x01 = CTAP, 0x02 = Update
		 *
		 * The protocol revision affects padding (revision 0 uses padding,
		 * revision 1 does not), but the message type prefix is always required.
		 * Per libwebauthn's implementation, the prefix is mandatory.
		 */
		framed_request[0] = 0x01;  /* MessageType::kCTAP */
		memcpy(framed_request + 1, ctap_request, ctap_request_len);
		request_len = ctap_request_len + 1;
		fprintf(stderr, "[DEBUG] Sending with MessageType prefix: 0x01 (CTAP), revision=%d\n",
				tunnel->noise.protocol_revision);

		if (cable_noise_encrypt(&tunnel->noise, framed_request, request_len,
								encrypted, &encrypted_len) < 0)
		{
			assertion->error_message = strdup("failed to encrypt request");
			goto cleanup;
		}

		if (cable_tunnel_send(tunnel, encrypted, encrypted_len) < 0)
		{
			assertion->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}
	}

	/* Receive and decrypt response */
	if (cable_tunnel_recv(tunnel, &response, &response_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
	{
		assertion->error_message = strdup(cable_tunnel_error(tunnel));
		goto cleanup;
	}

	{
		uint8_t		decrypted[1024];
		size_t		decrypted_len;
		uint8_t		status;

		if (cable_noise_decrypt(&tunnel->noise, response, response_len,
								decrypted, &decrypted_len) < 0)
		{
			assertion->error_message = strdup("failed to decrypt response");
			goto cleanup;
		}

		/*
		 * Response format: [MessageType: 1 byte] [CTAP status: 1 byte] [CBOR data]
		 *
		 * All responses include a message type prefix. The protocol revision
		 * only affects padding behavior, not the message framing.
		 */
		{
			size_t		header_len = 2;
			size_t		status_offset = 1;

			if (decrypted_len < 2)
			{
				assertion->error_message = strdup("CTAP2 response too short");
				goto cleanup;
			}
			if (decrypted[0] != 0x01)  /* MessageType::kCTAP */
			{
				char		err_msg[64];

				snprintf(err_msg, sizeof(err_msg), "unexpected MessageType: 0x%02x", decrypted[0]);
				assertion->error_message = strdup(err_msg);
				goto cleanup;
			}

			status = decrypted[status_offset];
			if (status != 0x00)		/* CTAP2_OK */
			{
				char		err_msg[64];

				snprintf(err_msg, sizeof(err_msg), "CTAP2 error: 0x%02x", status);
				assertion->error_message = strdup(err_msg);
				goto cleanup;
			}

			/* Parse assertion response (skip header bytes) */
			if (cable_cbor_decode_assertion_response(decrypted + header_len, decrypted_len - header_len,
													 &assertion->authenticator_data,
													 &assertion->authenticator_data_len,
													 &assertion->signature,
													 &assertion->signature_len,
													 &assertion->credential_id,
													 &assertion->credential_id_len) < 0)
			{
				assertion->error_message = strdup("failed to parse assertion response");
				goto cleanup;
			}
		}
	}

	/* Copy clientDataJSON */
	assertion->client_data_json = malloc(json_len);
	if (assertion->client_data_json)
	{
		memcpy(assertion->client_data_json, client_data_json, json_len);
		assertion->client_data_json_len = json_len;
	}

cleanup:
	free(fido_url);
	free(ctap_request);
	free(response);
	cable_tunnel_free(tunnel);

	/* Clear sensitive data */
	explicit_bzero(private_key, sizeof(private_key));
	explicit_bzero(&qr_data, sizeof(qr_data));

	return assertion;
}

/*
 * caBLE MakeCredential flow for passkey registration.
 *
 * Similar to cable_get_assertion but:
 * 1. Uses request_type = CABLE_REQUEST_TYPE_MAKE_CREDENTIAL in QR
 * 2. Sends CTAP2 MakeCredential (0x01) command instead of GetAssertion
 * 3. Returns attestation with credential_id and public_key
 */
PasskeyAttestation *
cable_make_credential(const char *rp_id,
					  const char *rp_name,
					  const uint8_t *user_id,
					  size_t user_id_len,
					  const char *user_name,
					  const char *user_display_name,
					  const uint8_t *challenge,
					  size_t challenge_len)
{
	CableQRData qr_data;
	CableTunnel *tunnel = NULL;
	PasskeyAttestation *attestation = NULL;
	char	   *fido_url = NULL;
	uint8_t		private_key[CABLE_P256_PRIVKEY_LENGTH];
	uint8_t		client_data_hash[32];
	uint8_t	   *ctap_request = NULL;
	size_t		ctap_request_len;
	uint8_t	   *response = NULL;
	size_t		response_len;
	char		client_data_json[512];
	int			json_len;
	uint8_t		advert_plaintext[16];
	bool		have_advert_plaintext = false;

	attestation = calloc(1, sizeof(PasskeyAttestation));
	if (!attestation)
		return NULL;

	/* Generate P-256 key pair */
	if (cable_generate_keypair(qr_data.peer_identity, private_key) < 0)
	{
		attestation->error_message = strdup("failed to generate key pair");
		goto cleanup;
	}

	/* Generate random secret */
	if (cable_generate_secret(qr_data.secret, CABLE_SECRET_LENGTH) < 0)
	{
		attestation->error_message = strdup("failed to generate secret");
		goto cleanup;
	}

	/* Set QR data fields - MakeCredential operation */
	qr_data.known_domains = 2;
	qr_data.request_type = CABLE_REQUEST_TYPE_MAKE_CREDENTIAL;

	/* Generate FIDO:/ URL and display QR code */
	fido_url = cable_generate_qr_url(&qr_data);
	if (!fido_url)
	{
		attestation->error_message = strdup("failed to generate QR code URL");
		goto cleanup;
	}

	fprintf(stderr, "[DEBUG] Generated FIDO URL for registration: %s\n", fido_url);
	fprintf(stderr, "[DEBUG] request_type = mc (MakeCredential)\n");

	cable_display_qr(fido_url);

	/* Create tunnel and connect to server */
	tunnel = cable_tunnel_new();
	if (!tunnel)
	{
		attestation->error_message = strdup("failed to create tunnel");
		goto cleanup;
	}

	/* Derive tunnel ID and connect (same as cable_get_assertion) */
	{
		uint8_t		tunnel_id[CABLE_TUNNEL_ID_LENGTH];
		uint8_t		routing_id[CABLE_ROUTING_ID_LENGTH];
		uint8_t		eid_key[64];
		uint16_t	tunnel_domain;
		const char *tunnel_server;

		/* Initialize advert_plaintext */
		memset(advert_plaintext, 0, sizeof(advert_plaintext));

		/* Derive tunnel_id using HKDF-SHA256 */
		{
			uint8_t		info[4] = {2, 0, 0, 0};
			uint8_t		zero_salt[32] = {0};
			uint8_t		prk[32];
			uint8_t		expand_input[37];
			unsigned int prk_len = 32;
			unsigned int expand_len = 32;

			HMAC(EVP_sha256(), zero_salt, 32,
				 qr_data.secret, CABLE_SECRET_LENGTH,
				 prk, &prk_len);

			memcpy(expand_input, info, 4);
			expand_input[4] = 0x01;
			HMAC(EVP_sha256(), prk, 32,
				 expand_input, 5,
				 tunnel_id, &expand_len);
		}

		fprintf(stderr, "[DEBUG] Derived tunnel_id (hex): ");
		for (int i = 0; i < CABLE_TUNNEL_ID_LENGTH; i++)
			fprintf(stderr, "%02x", tunnel_id[i]);
		fprintf(stderr, "\n");

		/* Try BLE or fallback to derived routing_id */
		if (getenv("PGPASSKEY_BLE") != NULL)
		{
			if (cable_derive_eid_key(qr_data.secret, eid_key) < 0)
			{
				attestation->error_message = strdup("failed to derive EID key");
				goto cleanup;
			}

			if (cable_ble_start_scan(eid_key, sizeof(eid_key)) < 0)
			{
				fprintf(stderr, "[BLE] Failed to start BLE scanner - falling back\n");
				explicit_bzero(eid_key, sizeof(eid_key));
				goto fallback_routing;
			}

			fprintf(stderr, "[DEBUG] Waiting for BLE advertisement from phone...\n");
			if (cable_ble_wait_for_advert(routing_id, &tunnel_domain, advert_plaintext,
										  CABLE_AUTH_TIMEOUT_SECS) < 0)
			{
				cable_ble_stop_scan();
				fprintf(stderr, "[BLE] BLE advertisement not received - falling back\n");
				explicit_bzero(eid_key, sizeof(eid_key));
				goto fallback_routing;
			}

			cable_ble_stop_scan();
			explicit_bzero(eid_key, sizeof(eid_key));
			have_advert_plaintext = true;

			tunnel_server = (tunnel_domain == CABLE_DOMAIN_ID_APPLE)
				? CABLE_TUNNEL_SERVER_APPLE
				: CABLE_TUNNEL_SERVER_GOOGLE;
		}
		else
		{
fallback_routing:
			{
				uint8_t		hash[32];

				fprintf(stderr, "[DEBUG] Using derived routing_id\n");
				SHA256(qr_data.peer_identity, sizeof(qr_data.peer_identity), hash);
				memcpy(routing_id, hash, CABLE_ROUTING_ID_LENGTH);
			}
			tunnel_server = CABLE_TUNNEL_SERVER_APPLE;
		}

		fprintf(stderr, "[DEBUG] Connecting to %s tunnel server...\n", tunnel_server);
		if (cable_tunnel_connect(tunnel, tunnel_server, tunnel_id, routing_id) < 0)
		{
			attestation->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}
	}

	/* Initialize Noise protocol and perform handshake */
	if (cable_noise_init(&tunnel->noise, private_key, qr_data.peer_identity) < 0)
	{
		attestation->error_message = strdup("failed to initialize Noise protocol");
		goto cleanup;
	}

	/*
	 * Derive keys for Noise handshake.
	 * Pass advert_plaintext if received via BLE, NULL otherwise.
	 */
	cable_noise_derive_keys(&tunnel->noise, qr_data.secret,
							have_advert_plaintext ? advert_plaintext : NULL);

	/* Perform Noise handshake */
	{
		uint8_t    *phone_msg = NULL;
		size_t		phone_msg_len;
		uint8_t		handshake_msg[CABLE_P256_PUBKEY_LENGTH + CABLE_GCM_TAG_LENGTH];
		size_t		handshake_len;

		fprintf(stderr, "[DEBUG] Starting Noise handshake for registration\n");

		if (cable_noise_handshake_start(&tunnel->noise, NULL, NULL,
										handshake_msg, &handshake_len) < 0)
		{
			attestation->error_message = strdup("failed to generate handshake");
			goto cleanup;
		}

		if (cable_tunnel_send(tunnel, handshake_msg, handshake_len) < 0)
		{
			attestation->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}

		if (cable_tunnel_recv(tunnel, &phone_msg, &phone_msg_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
		{
			attestation->error_message = strdup("failed to receive handshake from phone");
			goto cleanup;
		}

		if (cable_noise_process_peer_ephemeral(&tunnel->noise, phone_msg, phone_msg_len) < 0)
		{
			attestation->error_message = strdup("failed to process phone's handshake");
			free(phone_msg);
			goto cleanup;
		}
		free(phone_msg);

		if (cable_noise_complete_handshake(&tunnel->noise) < 0)
		{
			attestation->error_message = strdup("failed to complete handshake");
			goto cleanup;
		}
		fprintf(stderr, "[DEBUG] Noise handshake complete for registration\n");
	}

	/*
	 * Receive and process the post-handshake message from the phone.
	 * Per caBLE v2 protocol, after the Noise handshake the phone sends:
	 *   - Key 1: GetInfo response (authenticator capabilities)
	 *   - Key 2: Linking info (optional)
	 *   - Key 3: Features array (optional, e.g., "ctap", "dc")
	 * We need to consume this message before sending CTAP2 commands.
	 */
	{
		uint8_t    *post_msg = NULL;
		size_t		post_msg_len;
		uint8_t		decrypted[1024];
		size_t		decrypted_len;

		fprintf(stderr, "[DEBUG] Waiting for post-handshake message...\n");

		if (cable_tunnel_recv(tunnel, &post_msg, &post_msg_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
		{
			attestation->error_message = strdup("failed to receive post-handshake message");
			goto cleanup;
		}

		if (cable_noise_decrypt(&tunnel->noise, post_msg, post_msg_len,
								decrypted, &decrypted_len) < 0)
		{
			attestation->error_message = strdup("failed to decrypt post-handshake message");
			free(post_msg);
			goto cleanup;
		}
		free(post_msg);

		fprintf(stderr, "[DEBUG] Received post-handshake message (%zu bytes)\n", decrypted_len);
		fprintf(stderr, "[DEBUG] Post-handshake bytes:");
		for (size_t i = 0; i < decrypted_len && i < 128; i++)
		{
			if (i % 16 == 0)
				fprintf(stderr, "\n  ");
			fprintf(stderr, "%02x ", decrypted[i]);
		}
		fprintf(stderr, "\n");

		/*
		 * Detect protocol revision by checking for padding.
		 * Revision 0: message has null padding at end, followed by padding length byte
		 * Revision 1+: raw CBOR, no padding
		 *
		 * If revision 0, we do NOT use MessageType prefix on CTAP commands.
		 * If revision 1+, we DO use MessageType prefix.
		 */
		if (decrypted_len > 0 && decrypted[decrypted_len - 1] > 0 &&
			decrypted[decrypted_len - 1] <= 16)
		{
			/* Check if last N bytes are all zeros (padding) */
			uint8_t pad_len = decrypted[decrypted_len - 1];
			bool has_padding = true;

			if (pad_len < decrypted_len)
			{
				for (size_t i = decrypted_len - pad_len; i < decrypted_len - 1; i++)
				{
					if (decrypted[i] != 0)
					{
						has_padding = false;
						break;
					}
				}
			}
			else
			{
				has_padding = false;
			}

			if (has_padding)
			{
				tunnel->noise.protocol_revision = 0;
				fprintf(stderr, "[DEBUG] Detected protocol revision 0 (padded message)\n");
			}
			else
			{
				tunnel->noise.protocol_revision = 1;
				fprintf(stderr, "[DEBUG] Detected protocol revision 1+ (no padding)\n");
			}
		}
		else
		{
			tunnel->noise.protocol_revision = 1;  /* Default to revision 1 */
			fprintf(stderr, "[DEBUG] Assuming protocol revision 1+\n");
		}
	}

	/* Build clientDataJSON for create operation */
	json_len = snprintf(client_data_json, sizeof(client_data_json),
						"{\"type\":\"webauthn.create\",\"challenge\":\"%.*s\",\"origin\":\"postgresql://%s\",\"crossOrigin\":false}",
						(int) challenge_len, challenge, rp_id);

	/* Compute clientDataHash */
	{
		SHA256_CTX	sha_ctx;

		SHA256_Init(&sha_ctx);
		SHA256_Update(&sha_ctx, client_data_json, json_len);
		SHA256_Final(client_data_hash, &sha_ctx);
	}

	/* Build CTAP2 MakeCredential request */
	ctap_request = cable_cbor_encode_make_credential(rp_id, rp_name,
													 user_id, user_id_len,
													 user_name, user_display_name,
													 client_data_hash,
													 &ctap_request_len);
	if (!ctap_request)
	{
		attestation->error_message = strdup("failed to encode CTAP2 request");
		goto cleanup;
	}

	fprintf(stderr, "[DEBUG] CTAP2 MakeCredential request (%zu bytes):", ctap_request_len);
	for (size_t i = 0; i < ctap_request_len && i < 32; i++)
		fprintf(stderr, " %02x", ctap_request[i]);
	fprintf(stderr, "...\n");

	/* Encrypt and send request */
	{
		uint8_t		framed_request[1024];
		uint8_t		encrypted[1024];
		size_t		encrypted_len;
		size_t		request_len;

		/*
		 * All CTAP messages are prefixed with a type byte:
		 *   0x00 = Shutdown, 0x01 = CTAP, 0x02 = Update
		 *
		 * The protocol revision affects padding (revision 0 uses padding,
		 * revision 1 does not), but the message type prefix is always required.
		 * Per libwebauthn's implementation, the prefix is mandatory.
		 */
		framed_request[0] = 0x01;  /* MessageType::kCTAP */
		memcpy(framed_request + 1, ctap_request, ctap_request_len);
		request_len = ctap_request_len + 1;
		fprintf(stderr, "[DEBUG] Sending with MessageType prefix: 0x01 (CTAP), revision=%d\n",
				tunnel->noise.protocol_revision);

		if (cable_noise_encrypt(&tunnel->noise, framed_request, request_len,
								encrypted, &encrypted_len) < 0)
		{
			attestation->error_message = strdup("failed to encrypt request");
			goto cleanup;
		}

		if (cable_tunnel_send(tunnel, encrypted, encrypted_len) < 0)
		{
			attestation->error_message = strdup(cable_tunnel_error(tunnel));
			goto cleanup;
		}
	}

	/* Receive and decrypt response */
	if (cable_tunnel_recv(tunnel, &response, &response_len, CABLE_AUTH_TIMEOUT_SECS * 1000) < 0)
	{
		attestation->error_message = strdup(cable_tunnel_error(tunnel));
		goto cleanup;
	}

	{
		uint8_t		decrypted[1024];
		size_t		decrypted_len;
		uint8_t		status;

		if (cable_noise_decrypt(&tunnel->noise, response, response_len,
								decrypted, &decrypted_len) < 0)
		{
			attestation->error_message = strdup("failed to decrypt response");
			goto cleanup;
		}

		fprintf(stderr, "[DEBUG] Decrypted response (%zu bytes):", decrypted_len);
		for (size_t i = 0; i < decrypted_len && i < 32; i++)
			fprintf(stderr, " %02x", decrypted[i]);
		fprintf(stderr, "%s\n", decrypted_len > 32 ? "..." : "");

		/*
		 * Response format: [MessageType: 1 byte] [CTAP status: 1 byte] [CBOR data]
		 *
		 * All responses include a message type prefix. The protocol revision
		 * only affects padding behavior, not the message framing.
		 */
		{
			size_t		header_len = 2;
			size_t		status_offset = 1;

			if (decrypted_len < 2)
			{
				attestation->error_message = strdup("CTAP2 response too short");
				goto cleanup;
			}
			if (decrypted[0] != 0x01)  /* MessageType::kCTAP */
			{
				char		err_msg[64];

				snprintf(err_msg, sizeof(err_msg), "unexpected MessageType: 0x%02x", decrypted[0]);
				attestation->error_message = strdup(err_msg);
				goto cleanup;
			}

			status = decrypted[status_offset];
			if (status != 0x00)		/* CTAP2_OK */
			{
				char		err_msg[64];

				snprintf(err_msg, sizeof(err_msg), "CTAP2 error: 0x%02x", status);
				attestation->error_message = strdup(err_msg);
				goto cleanup;
			}

			/* Parse attestation response (skip header bytes) */
			if (cable_cbor_decode_attestation_response(decrypted + header_len, decrypted_len - header_len,
													   &attestation->authenticator_data,
													   &attestation->authenticator_data_len,
													   &attestation->credential_id,
													   &attestation->credential_id_len,
													   &attestation->public_key,
													   &attestation->public_key_len) < 0)
			{
				attestation->error_message = strdup("failed to parse attestation response");
				goto cleanup;
			}
		}
	}

	/* Copy clientDataJSON */
	attestation->client_data_json = malloc(json_len);
	if (attestation->client_data_json)
	{
		memcpy(attestation->client_data_json, client_data_json, json_len);
		attestation->client_data_json_len = json_len;
	}

	fprintf(stderr, "[DEBUG] Registration successful! credential_id_len=%zu, pubkey_len=%zu\n",
			attestation->credential_id_len, attestation->public_key_len);

cleanup:
	free(fido_url);
	free(ctap_request);
	free(response);
	cable_tunnel_free(tunnel);

	/* Clear sensitive data */
	explicit_bzero(private_key, sizeof(private_key));
	explicit_bzero(&qr_data, sizeof(qr_data));

	return attestation;
}

void
cable_free_attestation(PasskeyAttestation *attestation)
{
	if (!attestation)
		return;
	free(attestation->authenticator_data);
	free(attestation->client_data_json);
	free(attestation->credential_id);
	free(attestation->public_key);
	free(attestation->error_message);
	free(attestation);
}

#else							/* !USE_OPENSSL */

/* Stubs when OpenSSL is not available */

int
cable_generate_keypair(uint8_t *public_key, uint8_t *private_key)
{
	return -1;
}

int
cable_generate_secret(uint8_t *secret, size_t len)
{
	return -1;
}

char *
cable_generate_qr_url(const CableQRData *data)
{
	return NULL;
}

void
cable_display_qr(const char *fido_url)
{
	fprintf(stderr, "caBLE requires OpenSSL support\n");
}

PasskeyAssertion *
cable_get_assertion(const char *rp_id,
					const uint8_t *challenge,
					size_t challenge_len,
					const uint8_t *credential_id,
					size_t credential_id_len)
{
	PasskeyAssertion *assertion = calloc(1, sizeof(PasskeyAssertion));

	if (assertion)
		assertion->error_message = strdup("caBLE requires OpenSSL support");
	return assertion;
}

PasskeyAttestation *
cable_make_credential(const char *rp_id,
					  const char *rp_name,
					  const uint8_t *user_id,
					  size_t user_id_len,
					  const char *user_name,
					  const char *user_display_name,
					  const uint8_t *challenge,
					  size_t challenge_len)
{
	PasskeyAttestation *attestation = calloc(1, sizeof(PasskeyAttestation));

	if (attestation)
		attestation->error_message = strdup("caBLE requires OpenSSL support");
	return attestation;
}

void
cable_free_attestation(PasskeyAttestation *attestation)
{
	if (!attestation)
		return;
	free(attestation->authenticator_data);
	free(attestation->client_data_json);
	free(attestation->credential_id);
	free(attestation->public_key);
	free(attestation->error_message);
	free(attestation);
}

#endif							/* USE_OPENSSL */
