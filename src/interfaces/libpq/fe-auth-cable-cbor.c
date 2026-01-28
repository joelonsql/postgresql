/*-------------------------------------------------------------------------
 * fe-auth-cable-cbor.c
 *	  Minimal CBOR encoder for caBLE protocol
 *
 * This implements a minimal subset of CBOR (RFC 8949) needed for caBLE:
 * - Unsigned integers
 * - Negative integers
 * - Byte strings
 * - Text strings
 * - Maps
 * - Booleans
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable-cbor.c
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include "libpq/cable.h"

/* CBOR major types */
#define CBOR_UINT		0x00
#define CBOR_NEGINT		0x20
#define CBOR_BYTES		0x40
#define CBOR_TEXT		0x60
#define CBOR_ARRAY		0x80
#define CBOR_MAP		0xA0
#define CBOR_TAG		0xC0
#define CBOR_SIMPLE		0xE0

/* CBOR simple values */
#define CBOR_FALSE		0xF4
#define CBOR_TRUE		0xF5
#define CBOR_NULL		0xF6

/*
 * CBOR encoder state
 */
typedef struct CborEncoder
{
	uint8_t	   *buffer;
	size_t		capacity;
	size_t		offset;
	bool		error;
} CborEncoder;

static void
cbor_encoder_init(CborEncoder *enc, uint8_t *buffer, size_t capacity)
{
	enc->buffer = buffer;
	enc->capacity = capacity;
	enc->offset = 0;
	enc->error = false;
}

static void
cbor_write_byte(CborEncoder *enc, uint8_t byte)
{
	if (enc->offset >= enc->capacity)
	{
		enc->error = true;
		return;
	}
	enc->buffer[enc->offset++] = byte;
}

static void
cbor_write_bytes(CborEncoder *enc, const uint8_t *bytes, size_t len)
{
	if (enc->offset + len > enc->capacity)
	{
		enc->error = true;
		return;
	}
	memcpy(enc->buffer + enc->offset, bytes, len);
	enc->offset += len;
}

/*
 * Encode an unsigned integer with the given major type.
 */
static void
cbor_encode_uint_type(CborEncoder *enc, uint8_t major_type, uint64_t value)
{
	if (value < 24)
	{
		cbor_write_byte(enc, major_type | (uint8_t) value);
	}
	else if (value <= 0xFF)
	{
		cbor_write_byte(enc, major_type | 24);
		cbor_write_byte(enc, (uint8_t) value);
	}
	else if (value <= 0xFFFF)
	{
		cbor_write_byte(enc, major_type | 25);
		cbor_write_byte(enc, (value >> 8) & 0xFF);
		cbor_write_byte(enc, value & 0xFF);
	}
	else if (value <= 0xFFFFFFFF)
	{
		cbor_write_byte(enc, major_type | 26);
		cbor_write_byte(enc, (value >> 24) & 0xFF);
		cbor_write_byte(enc, (value >> 16) & 0xFF);
		cbor_write_byte(enc, (value >> 8) & 0xFF);
		cbor_write_byte(enc, value & 0xFF);
	}
	else
	{
		cbor_write_byte(enc, major_type | 27);
		cbor_write_byte(enc, (value >> 56) & 0xFF);
		cbor_write_byte(enc, (value >> 48) & 0xFF);
		cbor_write_byte(enc, (value >> 40) & 0xFF);
		cbor_write_byte(enc, (value >> 32) & 0xFF);
		cbor_write_byte(enc, (value >> 24) & 0xFF);
		cbor_write_byte(enc, (value >> 16) & 0xFF);
		cbor_write_byte(enc, (value >> 8) & 0xFF);
		cbor_write_byte(enc, value & 0xFF);
	}
}

static void
cbor_encode_uint(CborEncoder *enc, uint64_t value)
{
	cbor_encode_uint_type(enc, CBOR_UINT, value);
}

static void
cbor_encode_negint(CborEncoder *enc, int64_t value)
{
	/* CBOR negative integers encode -1-n */
	cbor_encode_uint_type(enc, CBOR_NEGINT, (uint64_t) (-1 - value));
}

static void
cbor_encode_bytes(CborEncoder *enc, const uint8_t *bytes, size_t len)
{
	cbor_encode_uint_type(enc, CBOR_BYTES, len);
	cbor_write_bytes(enc, bytes, len);
}

static void
cbor_encode_text(CborEncoder *enc, const char *text)
{
	size_t		len = strlen(text);

	cbor_encode_uint_type(enc, CBOR_TEXT, len);
	cbor_write_bytes(enc, (const uint8_t *) text, len);
}

static void
cbor_encode_map_start(CborEncoder *enc, size_t num_pairs)
{
	cbor_encode_uint_type(enc, CBOR_MAP, num_pairs);
}

static void
cbor_encode_bool(CborEncoder *enc, bool value)
{
	cbor_write_byte(enc, value ? CBOR_TRUE : CBOR_FALSE);
}

/*
 * Compress a P-256 public key from uncompressed (65 bytes) to compressed (33 bytes).
 * Uncompressed format: 04 || X (32 bytes) || Y (32 bytes)
 * Compressed format: (02 or 03) || X (32 bytes)
 * The prefix is 02 if Y is even, 03 if Y is odd.
 */
static void
compress_p256_pubkey(const uint8_t *uncompressed, uint8_t *compressed)
{
	/* Check that input is uncompressed format (starts with 0x04) */
	if (uncompressed[0] != 0x04)
	{
		/* Already compressed or invalid - just copy first 33 bytes */
		memcpy(compressed, uncompressed, 33);
		return;
	}

	/* The Y coordinate is the last 32 bytes */
	/* If Y's last byte is even, prefix is 0x02; if odd, prefix is 0x03 */
	compressed[0] = (uncompressed[64] & 1) ? 0x03 : 0x02;

	/* Copy X coordinate (bytes 1-32 of uncompressed) */
	memcpy(compressed + 1, uncompressed + 1, 32);
}

/*
 * Encode the caBLE HandshakeV2 message.
 *
 * CBOR map structure (per Chromium/iOS caBLE implementation):
 *   0: peer_identity (33 bytes) - Compressed P-256 public key
 *   1: secret (16 bytes) - Symmetric secret
 *   2: known_domains (uint) - Number of tunnel server domains we know (2 = both)
 *   3: timestamp (uint) - Unix epoch seconds
 *   4: supports_linking (bool) - Whether we support device linking (false for now)
 *   5: request_type (text) - Operation hint ("ga"=GetAssertion, "mc"=MakeCredential)
 *
 * Returns allocated buffer that caller must free, or NULL on error.
 */
uint8_t *
cable_cbor_encode_handshake(const CableQRData *data, size_t *out_len)
{
	CborEncoder enc;
	uint8_t	   *buffer;
	size_t		capacity = 256;	/* More than enough for handshake */
	uint8_t		compressed_pubkey[33];

	buffer = malloc(capacity);
	if (!buffer)
		return NULL;

	cbor_encoder_init(&enc, buffer, capacity);

	/* Compress the public key */
	compress_p256_pubkey(data->peer_identity, compressed_pubkey);

	/* Map with 6 entries (all required for iOS compatibility) */
	cbor_encode_map_start(&enc, 6);

	/* 0: peer_identity (compressed, 33 bytes) */
	cbor_encode_uint(&enc, 0);
	cbor_encode_bytes(&enc, compressed_pubkey, 33);

	/* 1: secret (16 bytes) */
	cbor_encode_uint(&enc, 1);
	cbor_encode_bytes(&enc, data->secret, CABLE_SECRET_LENGTH);

	/* 2: known_domains (number of tunnel domains we know: 2 = Google + Apple) */
	cbor_encode_uint(&enc, 2);
	cbor_encode_uint(&enc, data->known_domains);

	/* 3: timestamp (Unix epoch seconds) */
	cbor_encode_uint(&enc, 3);
	cbor_encode_uint(&enc, (uint64_t) time(NULL));

	/* 4: supports_linking (false - we don't support device pairing) */
	cbor_encode_uint(&enc, 4);
	cbor_encode_bool(&enc, false);

	/* 5: request_type / operation hint ("ga"=GetAssertion, "mc"=MakeCredential) */
	cbor_encode_uint(&enc, 5);
	if (data->request_type == CABLE_REQUEST_TYPE_GET_ASSERTION)
		cbor_encode_text(&enc, "ga");
	else
		cbor_encode_text(&enc, "mc");

	if (enc.error)
	{
		free(buffer);
		return NULL;
	}

	*out_len = enc.offset;

	/* Debug output */
	{
		uint64_t	ts = (uint64_t) time(NULL);
		int			i;

		fprintf(stderr, "[DEBUG] CBOR handshake encoded (%zu bytes):\n", enc.offset);
		fprintf(stderr, "[DEBUG]   0: pubkey[33] = %02x%02x%02x%02x...\n",
				compressed_pubkey[0], compressed_pubkey[1],
				compressed_pubkey[2], compressed_pubkey[3]);
		fprintf(stderr, "[DEBUG]   1: secret[16]\n");
		fprintf(stderr, "[DEBUG]   2: known_domains = %u\n", data->known_domains);
		fprintf(stderr, "[DEBUG]   3: timestamp = %" PRIu64 "\n", ts);
		fprintf(stderr, "[DEBUG]   4: supports_linking = false\n");
		fprintf(stderr, "[DEBUG]   5: request_type = \"%s\"\n",
				data->request_type == CABLE_REQUEST_TYPE_GET_ASSERTION ? "ga" : "mc");
		fprintf(stderr, "[DEBUG] Raw CBOR: ");
		for (i = 0; i < (int) enc.offset && i < 32; i++)
			fprintf(stderr, "%02x", buffer[i]);
		if (enc.offset > 32)
			fprintf(stderr, "...");
		fprintf(stderr, "\n");
	}

	return buffer;
}

/*
 * Encode an array start header.
 */
static void
cbor_encode_array_start(CborEncoder *enc, size_t num_elements)
{
	cbor_encode_uint_type(enc, CBOR_ARRAY, num_elements);
}

/*
 * Encode a CTAP2 GetAssertion command.
 *
 * Command structure:
 *   Command byte: 0x02 (authenticatorGetAssertion)
 *   CBOR map:
 *     0x01 (rpId): text string
 *     0x02 (clientDataHash): bytes(32)
 *     0x03 (allowCredentials): array of PublicKeyCredentialDescriptor (optional)
 *     0x05 (options): map { "up": true, "uv": true }
 *
 * The allowCredentials field (0x03) is included when credential_id is provided.
 * Each PublicKeyCredentialDescriptor is a map with text keys "id" and "type".
 *
 * Returns allocated buffer that caller must free, or NULL on error.
 */
uint8_t *
cable_cbor_encode_get_assertion(const char *rp_id,
								const uint8_t *client_data_hash,
								const uint8_t *credential_id,
								size_t credential_id_len,
								size_t *out_len)
{
	CborEncoder enc;
	uint8_t	   *buffer;
	size_t		capacity = 512;
	int			map_entries;

	buffer = malloc(capacity);
	if (!buffer)
		return NULL;

	cbor_encoder_init(&enc, buffer, capacity);

	/* Command byte */
	cbor_write_byte(&enc, CTAP2_CMD_GET_ASSERTION);

	/* Map with 3 or 4 entries depending on credential_id */
	map_entries = (credential_id && credential_id_len > 0) ? 4 : 3;
	cbor_encode_map_start(&enc, map_entries);

	/* 0x01: rpId */
	cbor_encode_uint(&enc, CTAP2_GA_RPID);
	cbor_encode_text(&enc, rp_id);

	/* 0x02: clientDataHash (32 bytes) */
	cbor_encode_uint(&enc, CTAP2_GA_CLIENT_DATA_HASH);
	cbor_encode_bytes(&enc, client_data_hash, 32);

	/* 0x03: allowCredentials (if credential_id provided) */
	if (credential_id && credential_id_len > 0)
	{
		cbor_encode_uint(&enc, CTAP2_GA_ALLOW_LIST);
		cbor_encode_array_start(&enc, 1);	/* array with 1 credential */

		/* PublicKeyCredentialDescriptor: {"id": bytes, "type": "public-key"} */
		cbor_encode_map_start(&enc, 2);
		cbor_encode_text(&enc, "id");		/* Field name as text string */
		cbor_encode_bytes(&enc, credential_id, credential_id_len);
		cbor_encode_text(&enc, "type");		/* Field name as text string */
		cbor_encode_text(&enc, "public-key");
	}

	/* 0x05: options { "up": true, "uv": true } */
	cbor_encode_uint(&enc, CTAP2_GA_OPTIONS);
	cbor_encode_map_start(&enc, 2);
	cbor_encode_text(&enc, "up");
	cbor_encode_bool(&enc, true);
	cbor_encode_text(&enc, "uv");
	cbor_encode_bool(&enc, true);

	if (enc.error)
	{
		free(buffer);
		return NULL;
	}

	*out_len = enc.offset;
	return buffer;
}

/*
 * CBOR decoder state
 */
typedef struct CborDecoder
{
	const uint8_t *buffer;
	size_t		length;
	size_t		offset;
	bool		error;
} CborDecoder;

static void
cbor_decoder_init(CborDecoder *dec, const uint8_t *buffer, size_t length)
{
	dec->buffer = buffer;
	dec->length = length;
	dec->offset = 0;
	dec->error = false;
}

static uint8_t
cbor_read_byte(CborDecoder *dec)
{
	if (dec->offset >= dec->length)
	{
		dec->error = true;
		return 0;
	}
	return dec->buffer[dec->offset++];
}

static uint64_t
cbor_decode_uint_value(CborDecoder *dec, uint8_t additional)
{
	uint64_t	value;

	if (additional < 24)
	{
		return additional;
	}
	else if (additional == 24)
	{
		return cbor_read_byte(dec);
	}
	else if (additional == 25)
	{
		value = (uint64_t) cbor_read_byte(dec) << 8;
		value |= cbor_read_byte(dec);
		return value;
	}
	else if (additional == 26)
	{
		value = (uint64_t) cbor_read_byte(dec) << 24;
		value |= (uint64_t) cbor_read_byte(dec) << 16;
		value |= (uint64_t) cbor_read_byte(dec) << 8;
		value |= cbor_read_byte(dec);
		return value;
	}
	else if (additional == 27)
	{
		value = (uint64_t) cbor_read_byte(dec) << 56;
		value |= (uint64_t) cbor_read_byte(dec) << 48;
		value |= (uint64_t) cbor_read_byte(dec) << 40;
		value |= (uint64_t) cbor_read_byte(dec) << 32;
		value |= (uint64_t) cbor_read_byte(dec) << 24;
		value |= (uint64_t) cbor_read_byte(dec) << 16;
		value |= (uint64_t) cbor_read_byte(dec) << 8;
		value |= cbor_read_byte(dec);
		return value;
	}
	else
	{
		dec->error = true;
		return 0;
	}
}

/*
 * Decode and return the major type and value of the next CBOR item.
 * Does not consume the byte string/text content, only returns the length.
 */
static bool
cbor_decode_head(CborDecoder *dec, uint8_t *major_type, uint64_t *value)
{
	uint8_t		initial = cbor_read_byte(dec);

	if (dec->error)
		return false;

	*major_type = initial & 0xE0;
	*value = cbor_decode_uint_value(dec, initial & 0x1F);
	return !dec->error;
}

/*
 * Skip a CBOR item (used when we don't care about certain map entries).
 */
static void
cbor_skip_item(CborDecoder *dec)
{
	uint8_t		major_type;
	uint64_t	value;
	size_t		i;

	if (!cbor_decode_head(dec, &major_type, &value))
		return;

	switch (major_type)
	{
		case CBOR_UINT:
		case CBOR_NEGINT:
		case CBOR_SIMPLE:
			/* Already consumed */
			break;

		case CBOR_BYTES:
		case CBOR_TEXT:
			/* Skip the content bytes */
			if (dec->offset + value > dec->length)
			{
				dec->error = true;
				return;
			}
			dec->offset += value;
			break;

		case CBOR_ARRAY:
			/* Skip each array element */
			for (i = 0; i < value && !dec->error; i++)
				cbor_skip_item(dec);
			break;

		case CBOR_MAP:
			/* Skip each key-value pair */
			for (i = 0; i < value && !dec->error; i++)
			{
				cbor_skip_item(dec);	/* key */
				cbor_skip_item(dec);	/* value */
			}
			break;

		default:
			dec->error = true;
			break;
	}
}

/*
 * Decode a CTAP2 GetAssertion response.
 *
 * Response structure (after status byte):
 *   CBOR map:
 *     0x01 (credential): map { "id": bytes, "type": text }
 *     0x02 (authData): bytes
 *     0x03 (signature): bytes
 *     0x04 (user): map (optional)
 *     0x05 (numberOfCredentials): uint (optional)
 *
 * Returns 0 on success, -1 on error.
 * Caller must free the allocated output buffers.
 */
int
cable_cbor_decode_assertion_response(const uint8_t *data, size_t len,
									 uint8_t **auth_data, size_t *auth_data_len,
									 uint8_t **signature, size_t *signature_len,
									 uint8_t **credential_id, size_t *credential_id_len)
{
	CborDecoder dec;
	uint8_t		major_type;
	uint64_t	map_len;
	uint64_t	key;
	size_t		i;

	*auth_data = NULL;
	*auth_data_len = 0;
	*signature = NULL;
	*signature_len = 0;
	*credential_id = NULL;
	*credential_id_len = 0;

	cbor_decoder_init(&dec, data, len);

	/* Decode map header */
	if (!cbor_decode_head(&dec, &major_type, &map_len))
		return -1;
	if (major_type != CBOR_MAP)
		return -1;

	/* Iterate through map entries */
	for (i = 0; i < map_len && !dec.error; i++)
	{
		uint64_t	value_len;

		/* Decode key (should be uint) */
		if (!cbor_decode_head(&dec, &major_type, &key))
			break;
		if (major_type != CBOR_UINT)
		{
			cbor_skip_item(&dec);
			continue;
		}

		switch (key)
		{
			case CTAP2_GA_RESP_CREDENTIAL:
				/* Credential is a map with "id" and "type" */
				{
					uint64_t	cred_map_len;
					size_t		j;

					if (!cbor_decode_head(&dec, &major_type, &cred_map_len))
						break;
					if (major_type != CBOR_MAP)
					{
						dec.error = true;
						break;
					}

					for (j = 0; j < cred_map_len && !dec.error; j++)
					{
						uint64_t	text_len;
						char		key_name[16];

						/* Key should be text */
						if (!cbor_decode_head(&dec, &major_type, &text_len))
							break;
						if (major_type != CBOR_TEXT || text_len >= sizeof(key_name))
						{
							cbor_skip_item(&dec);
							continue;
						}

						/* Read key name */
						if (dec.offset + text_len > dec.length)
						{
							dec.error = true;
							break;
						}
						memcpy(key_name, dec.buffer + dec.offset, text_len);
						key_name[text_len] = '\0';
						dec.offset += text_len;

						if (strcmp(key_name, "id") == 0)
						{
							/* Credential ID is a byte string */
							if (!cbor_decode_head(&dec, &major_type, &value_len))
								break;
							if (major_type != CBOR_BYTES)
							{
								dec.error = true;
								break;
							}
							if (dec.offset + value_len > dec.length)
							{
								dec.error = true;
								break;
							}
							*credential_id = malloc(value_len);
							if (!*credential_id)
							{
								dec.error = true;
								break;
							}
							memcpy(*credential_id, dec.buffer + dec.offset, value_len);
							*credential_id_len = value_len;
							dec.offset += value_len;
						}
						else
						{
							/* Skip other credential fields */
							cbor_skip_item(&dec);
						}
					}
				}
				break;

			case CTAP2_GA_RESP_AUTH_DATA:
				/* Authenticator data is a byte string */
				if (!cbor_decode_head(&dec, &major_type, &value_len))
					break;
				if (major_type != CBOR_BYTES)
				{
					dec.error = true;
					break;
				}
				if (dec.offset + value_len > dec.length)
				{
					dec.error = true;
					break;
				}
				*auth_data = malloc(value_len);
				if (!*auth_data)
				{
					dec.error = true;
					break;
				}
				memcpy(*auth_data, dec.buffer + dec.offset, value_len);
				*auth_data_len = value_len;
				dec.offset += value_len;
				break;

			case CTAP2_GA_RESP_SIGNATURE:
				/* Signature is a byte string */
				if (!cbor_decode_head(&dec, &major_type, &value_len))
					break;
				if (major_type != CBOR_BYTES)
				{
					dec.error = true;
					break;
				}
				if (dec.offset + value_len > dec.length)
				{
					dec.error = true;
					break;
				}
				*signature = malloc(value_len);
				if (!*signature)
				{
					dec.error = true;
					break;
				}
				memcpy(*signature, dec.buffer + dec.offset, value_len);
				*signature_len = value_len;
				dec.offset += value_len;
				break;

			default:
				/* Skip unknown keys */
				cbor_skip_item(&dec);
				break;
		}
	}

	if (dec.error)
	{
		free(*auth_data);
		free(*signature);
		free(*credential_id);
		*auth_data = NULL;
		*signature = NULL;
		*credential_id = NULL;
		return -1;
	}

	/* Verify we got the required fields */
	if (!*auth_data || !*signature)
	{
		free(*auth_data);
		free(*signature);
		free(*credential_id);
		*auth_data = NULL;
		*signature = NULL;
		*credential_id = NULL;
		return -1;
	}

	return 0;
}

/*
 * Encode a CTAP2 MakeCredential command.
 *
 * Command structure:
 *   Command byte: 0x01 (authenticatorMakeCredential)
 *   CBOR map:
 *     0x01 (clientDataHash): bytes(32)
 *     0x02 (rp): map { "id": text, "name": text }
 *     0x03 (user): map { "id": bytes, "name": text, "displayName": text }
 *     0x04 (pubKeyCredParams): array [ { "type": "public-key", "alg": -7 } ]
 *     0x07 (options): map { "rk": true, "uv": true }
 *
 * Returns allocated buffer that caller must free, or NULL on error.
 */
uint8_t *
cable_cbor_encode_make_credential(const char *rp_id,
								  const char *rp_name,
								  const uint8_t *user_id,
								  size_t user_id_len,
								  const char *user_name,
								  const char *user_display_name,
								  const uint8_t *client_data_hash,
								  size_t *out_len)
{
	CborEncoder enc;
	uint8_t	   *buffer;
	size_t		capacity = 1024;

	buffer = malloc(capacity);
	if (!buffer)
		return NULL;

	cbor_encoder_init(&enc, buffer, capacity);

	/* Command byte */
	cbor_write_byte(&enc, CTAP2_CMD_MAKE_CREDENTIAL);

	/* Map with 5 entries: clientDataHash, rp, user, pubKeyCredParams, options */
	cbor_encode_map_start(&enc, 5);

	/* 0x01: clientDataHash (32 bytes) */
	cbor_encode_uint(&enc, CTAP2_MC_CLIENT_DATA_HASH);
	cbor_encode_bytes(&enc, client_data_hash, 32);

	/* 0x02: rp { "id": text, "name": text } */
	cbor_encode_uint(&enc, CTAP2_MC_RP);
	cbor_encode_map_start(&enc, 2);
	cbor_encode_text(&enc, "id");
	cbor_encode_text(&enc, rp_id);
	cbor_encode_text(&enc, "name");
	cbor_encode_text(&enc, rp_name);

	/* 0x03: user { "id": bytes, "name": text, "displayName": text } */
	cbor_encode_uint(&enc, CTAP2_MC_USER);
	cbor_encode_map_start(&enc, 3);
	cbor_encode_text(&enc, "id");
	cbor_encode_bytes(&enc, user_id, user_id_len);
	cbor_encode_text(&enc, "name");
	cbor_encode_text(&enc, user_name);
	cbor_encode_text(&enc, "displayName");
	cbor_encode_text(&enc, user_display_name);

	/* 0x04: pubKeyCredParams [ { "type": "public-key", "alg": -7 } ] */
	cbor_encode_uint(&enc, CTAP2_MC_PUB_KEY_CRED_PARAMS);
	cbor_encode_uint_type(&enc, CBOR_ARRAY, 1);	/* Array of 1 element */
	cbor_encode_map_start(&enc, 2);
	cbor_encode_text(&enc, "type");
	cbor_encode_text(&enc, "public-key");
	cbor_encode_text(&enc, "alg");
	cbor_encode_negint(&enc, -7);	/* ES256 = COSE algorithm -7 */

	/* 0x07: options { "rk": true, "uv": true } */
	cbor_encode_uint(&enc, CTAP2_MC_OPTIONS);
	cbor_encode_map_start(&enc, 2);
	cbor_encode_text(&enc, "rk");
	cbor_encode_bool(&enc, true);
	cbor_encode_text(&enc, "uv");
	cbor_encode_bool(&enc, true);

	if (enc.error)
	{
		free(buffer);
		return NULL;
	}

	*out_len = enc.offset;
	return buffer;
}

/*
 * Decode a CTAP2 MakeCredential response.
 *
 * Response structure (after status byte):
 *   CBOR map:
 *     0x01 (fmt): text - attestation statement format (e.g., "none", "packed")
 *     0x02 (authData): bytes - authenticator data with credential
 *     0x03 (attStmt): map - attestation statement (may be empty for "none")
 *
 * The authData contains:
 *   rpIdHash(32) + flags(1) + counter(4) + aaguid(16) +
 *   credIdLen(2) + credentialId(credIdLen) + publicKey(COSE_Key)
 *
 * Returns 0 on success, -1 on error.
 * Caller must free the allocated output buffers.
 */
int
cable_cbor_decode_attestation_response(const uint8_t *data, size_t len,
									   uint8_t **auth_data, size_t *auth_data_len,
									   uint8_t **credential_id, size_t *credential_id_len,
									   uint8_t **public_key, size_t *public_key_len)
{
	CborDecoder dec;
	uint8_t		major_type;
	uint64_t	map_len;
	uint64_t	key;
	size_t		i;
	const uint8_t *auth_data_ptr = NULL;
	size_t		auth_data_size = 0;

	*auth_data = NULL;
	*auth_data_len = 0;
	*credential_id = NULL;
	*credential_id_len = 0;
	*public_key = NULL;
	*public_key_len = 0;

	cbor_decoder_init(&dec, data, len);

	/* Decode map header */
	if (!cbor_decode_head(&dec, &major_type, &map_len))
		return -1;
	if (major_type != CBOR_MAP)
		return -1;

	/* Iterate through map entries */
	for (i = 0; i < map_len && !dec.error; i++)
	{
		uint64_t	value_len;

		/* Decode key (should be uint) */
		if (!cbor_decode_head(&dec, &major_type, &key))
			break;
		if (major_type != CBOR_UINT)
		{
			cbor_skip_item(&dec);
			continue;
		}

		switch (key)
		{
			case CTAP2_MC_RESP_FMT:
				/* Attestation format - skip it */
				cbor_skip_item(&dec);
				break;

			case CTAP2_MC_RESP_AUTH_DATA:
				/* Authenticator data is a byte string */
				if (!cbor_decode_head(&dec, &major_type, &value_len))
					break;
				if (major_type != CBOR_BYTES)
				{
					dec.error = true;
					break;
				}
				if (dec.offset + value_len > dec.length)
				{
					dec.error = true;
					break;
				}
				auth_data_ptr = dec.buffer + dec.offset;
				auth_data_size = value_len;
				*auth_data = malloc(value_len);
				if (!*auth_data)
				{
					dec.error = true;
					break;
				}
				memcpy(*auth_data, auth_data_ptr, value_len);
				*auth_data_len = value_len;
				dec.offset += value_len;
				break;

			case CTAP2_MC_RESP_ATT_STMT:
				/* Attestation statement - skip it */
				cbor_skip_item(&dec);
				break;

			default:
				/* Skip unknown keys */
				cbor_skip_item(&dec);
				break;
		}
	}

	if (dec.error || !auth_data_ptr)
	{
		free(*auth_data);
		*auth_data = NULL;
		return -1;
	}

	/*
	 * Parse authenticator data to extract credential_id and public_key.
	 * Structure: rpIdHash(32) + flags(1) + counter(4) + aaguid(16) +
	 *            credIdLen(2) + credentialId + publicKey(COSE_Key)
	 */
	{
		const uint8_t *p = auth_data_ptr;
		const uint8_t *end = auth_data_ptr + auth_data_size;
		uint8_t		flags;
		uint16_t	cred_id_len;

		/* Skip rpIdHash (32 bytes) */
		if (p + 32 > end)
			goto parse_auth_error;
		p += 32;

		/* Read flags */
		if (p + 1 > end)
			goto parse_auth_error;
		flags = *p++;

		/* Verify AT flag is set (attested credential data) */
		if (!(flags & 0x40))	/* PASSKEY_FLAG_AT = 0x40 */
			goto parse_auth_error;

		/* Skip counter (4 bytes) */
		if (p + 4 > end)
			goto parse_auth_error;
		p += 4;

		/* Skip AAGUID (16 bytes) */
		if (p + 16 > end)
			goto parse_auth_error;
		p += 16;

		/* Read credential ID length (big-endian uint16) */
		if (p + 2 > end)
			goto parse_auth_error;
		cred_id_len = ((uint16_t) p[0] << 8) | p[1];
		p += 2;

		/* Read credential ID */
		if (p + cred_id_len > end)
			goto parse_auth_error;
		*credential_id = malloc(cred_id_len);
		if (!*credential_id)
			goto parse_auth_error;
		memcpy(*credential_id, p, cred_id_len);
		*credential_id_len = cred_id_len;
		p += cred_id_len;

		/*
		 * Parse COSE_Key to extract x and y coordinates for EC public key.
		 * COSE_Key is a CBOR map with keys for kty, alg, crv, x, y.
		 */
		{
			uint8_t		x_coord[32];
			uint8_t		y_coord[32];
			bool		found_x = false;
			bool		found_y = false;

			/* Simple scan for -2 (x) and -3 (y) keys */
			while (p < end - 34)
			{
				uint8_t		cbor_key = *p++;

				/* Key -2 (x coordinate) encoded as CBOR negative int: 0x21 */
				if (cbor_key == 0x21)
				{
					/* Next should be bytes header for 32-byte value */
					if (*p == 0x58 && p[1] == 32)
					{
						p += 2;
						memcpy(x_coord, p, 32);
						p += 32;
						found_x = true;
					}
				}
				/* Key -3 (y coordinate) encoded as CBOR negative int: 0x22 */
				else if (cbor_key == 0x22)
				{
					if (*p == 0x58 && p[1] == 32)
					{
						p += 2;
						memcpy(y_coord, p, 32);
						p += 32;
						found_y = true;
					}
				}

				if (found_x && found_y)
					break;
			}

			if (!found_x || !found_y)
				goto parse_auth_error;

			/* Build uncompressed EC point: 04 || x || y */
			*public_key = malloc(65);
			if (!*public_key)
				goto parse_auth_error;
			(*public_key)[0] = 0x04;
			memcpy(*public_key + 1, x_coord, 32);
			memcpy(*public_key + 33, y_coord, 32);
			*public_key_len = 65;
		}
	}

	return 0;

parse_auth_error:
	free(*auth_data);
	free(*credential_id);
	free(*public_key);
	*auth_data = NULL;
	*credential_id = NULL;
	*public_key = NULL;
	return -1;
}
