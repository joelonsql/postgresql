/*-------------------------------------------------------------------------
 * cable.h
 *	  caBLE (Cloud-Assisted Bluetooth Low Energy) hybrid transport definitions
 *
 * caBLE enables cross-device WebAuthn authentication by establishing an
 * encrypted tunnel between a client (psql) and a phone authenticator via
 * a cloud relay server. The phone scans a QR code displayed by the client
 * to initiate the connection.
 *
 * This implementation follows the CTAP 2.2 hybrid transport specification
 * and is compatible with iOS and Android authenticators.
 *
 * References:
 * - CTAP 2.2 Hybrid Transport: https://fidoalliance.org/specs/fido-v2.2-rd-20230321/
 * - Chromium caBLE: https://chromium.googlesource.com/chromium/src/+/HEAD/device/fido/cable/
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/cable.h
 *-------------------------------------------------------------------------
 */
#ifndef CABLE_H
#define CABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Tunnel server domains.
 * iOS devices use cable.auth.com (Apple), Android uses cable.ua5v.com (Google).
 * The client must support both to work with all devices.
 */
#define CABLE_TUNNEL_SERVER_GOOGLE	"cable.ua5v.com"
#define CABLE_TUNNEL_SERVER_APPLE	"cable.auth.com"
#define CABLE_TUNNEL_PORT			443

/* Domain IDs used in QR code to hint which server to prefer */
#define CABLE_DOMAIN_ID_GOOGLE		0
#define CABLE_DOMAIN_ID_APPLE		1

/* WebSocket subprotocol for caBLE */
#define CABLE_WEBSOCKET_PROTOCOL	"fido.cable"

/*
 * Protocol version for caBLE handshake.
 * Version 1 is the initial caBLE protocol.
 */
#define CABLE_PROTOCOL_VERSION		1

/*
 * Cryptographic constants
 */
#define CABLE_P256_PUBKEY_LENGTH	65		/* Uncompressed P-256 point */
#define CABLE_P256_PRIVKEY_LENGTH	32		/* P-256 private key */
#define CABLE_SECRET_LENGTH			16		/* Symmetric secret in QR */
#define CABLE_TUNNEL_ID_LENGTH		16		/* Tunnel identifier */
#define CABLE_ROUTING_ID_LENGTH		3		/* Routing ID for tunnel server */
#define CABLE_PSK_LENGTH			32		/* Pre-shared key length */
#define CABLE_NONCE_LENGTH			32		/* Nonce for key derivation */
#define CABLE_SESSION_KEY_LENGTH	32		/* AES-256 key length */
#define CABLE_GCM_TAG_LENGTH		16		/* AES-GCM authentication tag */
#define CABLE_GCM_NONCE_LENGTH		12		/* AES-GCM nonce length */

/*
 * QR code data structure (CBOR-encoded in QR code).
 * This is transmitted as a FIDO:/ URL with the CBOR bytes encoded as base10.
 *
 * CBOR map structure (HandshakeV2):
 *   0 (peer_identity): bytes(65) - Uncompressed P-256 public key
 *   1 (secret): bytes(16) - Symmetric secret for key derivation
 *   2 (known_domains): uint - Hint about preferred tunnel domain
 *   3 (request_type): uint - Type of request (GetAssertion=1)
 */
typedef struct CableQRData
{
	uint8_t		peer_identity[CABLE_P256_PUBKEY_LENGTH];	/* P-256 public key */
	uint8_t		secret[CABLE_SECRET_LENGTH];				/* Symmetric secret */
	uint8_t		known_domains;								/* Preferred domain hint */
	uint8_t		request_type;								/* 1=GetAssertion */
} CableQRData;

/* Request types for QR code */
#define CABLE_REQUEST_TYPE_GET_ASSERTION	1
#define CABLE_REQUEST_TYPE_MAKE_CREDENTIAL	2

/*
 * Noise protocol state.
 * caBLE uses a variant of the Noise_KNpsk0 pattern for the handshake.
 */
typedef struct CableNoiseState
{
	uint8_t		local_static_private[CABLE_P256_PRIVKEY_LENGTH];
	uint8_t		local_static_public[CABLE_P256_PUBKEY_LENGTH];
	uint8_t		local_ephemeral_private[CABLE_P256_PRIVKEY_LENGTH];
	uint8_t		local_ephemeral_public[CABLE_P256_PUBKEY_LENGTH];
	uint8_t		remote_static_public[CABLE_P256_PUBKEY_LENGTH];
	uint8_t		remote_ephemeral_public[CABLE_P256_PUBKEY_LENGTH];
	uint8_t		chaining_key[32];	/* Noise chaining key */
	uint8_t		handshake_hash[32]; /* h value */
	uint8_t		handshake_key[32];	/* k value for EncryptAndHash */
	uint32_t	handshake_nonce;	/* n value for EncryptAndHash */
	uint8_t		send_key[CABLE_SESSION_KEY_LENGTH];
	uint8_t		recv_key[CABLE_SESSION_KEY_LENGTH];
	uint32_t	send_counter;
	uint32_t	recv_counter;
	bool		handshake_complete;

	/* Stored peer ciphertext for DecryptAndHash after se ECDH */
	uint8_t		peer_ciphertext[CABLE_GCM_TAG_LENGTH];
	size_t		peer_ciphertext_len;

	/* Protocol revision (0 = no MessageType prefix, 1+ = with prefix) */
	int			protocol_revision;
} CableNoiseState;

/*
 * Tunnel connection state.
 */
typedef struct CableTunnel
{
	/* SSL/TLS connection state (opaque to avoid including OpenSSL headers) */
	void	   *ssl_ctx;
	void	   *ssl;
	int			socket_fd;

	/* Tunnel identification */
	uint8_t		tunnel_id[CABLE_TUNNEL_ID_LENGTH];
	uint8_t		routing_id[CABLE_ROUTING_ID_LENGTH];
	char	   *server_url;

	/* Noise protocol state for encrypted communication */
	CableNoiseState noise;

	/* WebSocket state */
	bool		ws_connected;
	uint8_t	   *ws_recv_buffer;
	size_t		ws_recv_buffer_size;
	size_t		ws_recv_buffer_len;

	/* Error handling */
	char	   *error_message;
} CableTunnel;

/*
 * CTAP2 command codes
 */
#define CTAP2_CMD_MAKE_CREDENTIAL		0x01
#define CTAP2_CMD_GET_ASSERTION			0x02
#define CTAP2_CMD_GET_INFO				0x04

/*
 * CTAP2 GetAssertion parameters (CBOR map keys)
 */
#define CTAP2_GA_RPID					0x01
#define CTAP2_GA_CLIENT_DATA_HASH		0x02
#define CTAP2_GA_ALLOW_LIST				0x03
#define CTAP2_GA_EXTENSIONS				0x04
#define CTAP2_GA_OPTIONS				0x05
#define CTAP2_GA_PIN_UV_AUTH_PARAM		0x06
#define CTAP2_GA_PIN_UV_AUTH_PROTOCOL	0x07

/*
 * CTAP2 GetAssertion response (CBOR map keys)
 */
#define CTAP2_GA_RESP_CREDENTIAL		0x01
#define CTAP2_GA_RESP_AUTH_DATA			0x02
#define CTAP2_GA_RESP_SIGNATURE			0x03
#define CTAP2_GA_RESP_USER				0x04
#define CTAP2_GA_RESP_NUMBER_OF_CREDS	0x05

/*
 * CTAP2 MakeCredential parameters (CBOR map keys)
 */
#define CTAP2_MC_CLIENT_DATA_HASH		0x01
#define CTAP2_MC_RP						0x02
#define CTAP2_MC_USER					0x03
#define CTAP2_MC_PUB_KEY_CRED_PARAMS	0x04
#define CTAP2_MC_EXCLUDE_LIST			0x05
#define CTAP2_MC_EXTENSIONS				0x06
#define CTAP2_MC_OPTIONS				0x07
#define CTAP2_MC_PIN_UV_AUTH_PARAM		0x08
#define CTAP2_MC_PIN_UV_AUTH_PROTOCOL	0x09

/*
 * CTAP2 MakeCredential response (CBOR map keys)
 */
#define CTAP2_MC_RESP_FMT				0x01
#define CTAP2_MC_RESP_AUTH_DATA			0x02
#define CTAP2_MC_RESP_ATT_STMT			0x03

/*
 * WebSocket opcodes (RFC 6455)
 */
#define WS_OPCODE_CONTINUATION	0x0
#define WS_OPCODE_TEXT			0x1
#define WS_OPCODE_BINARY		0x2
#define WS_OPCODE_CLOSE			0x8
#define WS_OPCODE_PING			0x9
#define WS_OPCODE_PONG			0xA

/*
 * Timeout configuration
 */
#define CABLE_CONNECT_TIMEOUT_SECS		10
#define CABLE_AUTH_TIMEOUT_SECS			60		/* Wait for phone to respond */
#define CABLE_POLL_INTERVAL_MS			100

/*
 * Function declarations - implemented in fe-auth-cable*.c
 */

/* Forward declaration for PasskeyAssertion (defined in fe-auth-passkey.h) */
struct PasskeyAssertion;
typedef struct PasskeyAssertion PasskeyAssertion;

/* QR code generation (fe-auth-cable.c) */
extern char *cable_generate_qr_url(const CableQRData *data);
extern void cable_display_qr(const char *fido_url);
extern int cable_generate_keypair(uint8_t *public_key, uint8_t *private_key);
extern int cable_generate_secret(uint8_t *secret, size_t len);

/* Full caBLE authentication flow */
extern PasskeyAssertion *cable_get_assertion(const char *rp_id,
											 const uint8_t *challenge,
											 size_t challenge_len,
											 const uint8_t *credential_id,
											 size_t credential_id_len);

/*
 * Passkey attestation result from MakeCredential operation.
 * Contains the newly created credential info.
 */
typedef struct PasskeyAttestation
{
	uint8_t	   *authenticator_data;
	size_t		authenticator_data_len;
	uint8_t	   *client_data_json;
	size_t		client_data_json_len;
	uint8_t	   *credential_id;
	size_t		credential_id_len;
	uint8_t	   *public_key;			/* 65-byte uncompressed EC point */
	size_t		public_key_len;
	char	   *error_message;
} PasskeyAttestation;

extern PasskeyAttestation *cable_make_credential(const char *rp_id,
												 const char *rp_name,
												 const uint8_t *user_id,
												 size_t user_id_len,
												 const char *user_name,
												 const char *user_display_name,
												 const uint8_t *challenge,
												 size_t challenge_len);
extern void cable_free_attestation(PasskeyAttestation *attestation);

/* CBOR encoding (fe-auth-cable-cbor.c) */
extern uint8_t *cable_cbor_encode_handshake(const CableQRData *data, size_t *out_len);
extern uint8_t *cable_cbor_encode_get_assertion(const char *rp_id,
												const uint8_t *client_data_hash,
												const uint8_t *credential_id,
												size_t credential_id_len,
												size_t *out_len);
extern uint8_t *cable_cbor_encode_make_credential(const char *rp_id,
												  const char *rp_name,
												  const uint8_t *user_id,
												  size_t user_id_len,
												  const char *user_name,
												  const char *user_display_name,
												  const uint8_t *client_data_hash,
												  size_t *out_len);
extern int cable_cbor_decode_assertion_response(const uint8_t *data, size_t len,
												uint8_t **auth_data, size_t *auth_data_len,
												uint8_t **signature, size_t *signature_len,
												uint8_t **credential_id, size_t *credential_id_len);
extern int cable_cbor_decode_attestation_response(const uint8_t *data, size_t len,
												  uint8_t **auth_data, size_t *auth_data_len,
												  uint8_t **credential_id, size_t *credential_id_len,
												  uint8_t **public_key, size_t *public_key_len);

/* Tunnel connection (fe-auth-cable-tunnel.c) */
extern CableTunnel *cable_tunnel_new(void);
extern void cable_tunnel_free(CableTunnel *tunnel);
extern int cable_tunnel_connect(CableTunnel *tunnel, const char *server,
								const uint8_t *tunnel_id, const uint8_t *routing_id);
extern int cable_tunnel_wait_for_peer(CableTunnel *tunnel, int timeout_secs);
extern int cable_tunnel_send(CableTunnel *tunnel, const uint8_t *data, size_t len);
extern int cable_tunnel_recv(CableTunnel *tunnel, uint8_t **data, size_t *len,
							 int timeout_ms);
extern const char *cable_tunnel_error(CableTunnel *tunnel);

/* Noise protocol (fe-auth-cable-noise.c) */
extern int cable_noise_init(CableNoiseState *state,
							const uint8_t *local_private,
							const uint8_t *local_public);
extern int cable_noise_handshake_start(CableNoiseState *state,
									   const uint8_t *psk,
									   const uint8_t *remote_public,
									   uint8_t *out_message, size_t *out_len);
extern int cable_noise_handshake_finish(CableNoiseState *state,
										const uint8_t *message, size_t len);
extern int cable_noise_process_peer_ephemeral(CableNoiseState *state,
											  const uint8_t *message, size_t len);
extern int cable_noise_complete_handshake(CableNoiseState *state);
extern int cable_noise_encrypt(CableNoiseState *state,
							   const uint8_t *plaintext, size_t plaintext_len,
							   uint8_t *ciphertext, size_t *ciphertext_len);
extern int cable_noise_decrypt(CableNoiseState *state,
							   const uint8_t *ciphertext, size_t ciphertext_len,
							   uint8_t *plaintext, size_t *plaintext_len);
extern void cable_noise_derive_keys(CableNoiseState *state,
									const uint8_t *secret,
									const uint8_t *advert_plaintext);

/* EID decryption (fe-auth-cable-eid.c) */
extern int cable_derive_eid_key(const uint8_t *qr_secret, uint8_t *eid_key);
extern int cable_eid_decrypt(const uint8_t *advert, const uint8_t *eid_key,
							 uint8_t *routing_id, uint16_t *tunnel_domain,
							 uint8_t *advert_plaintext);

/* BLE scanning (fe-auth-cable-ble-darwin.m on macOS) */
extern int cable_ble_start_scan(const uint8_t *eid_key, size_t key_len);
extern int cable_ble_wait_for_advert(uint8_t *routing_id, uint16_t *tunnel_domain,
									 uint8_t *advert_plaintext, int timeout_secs);
extern void cable_ble_stop_scan(void);

#endif							/* CABLE_H */
