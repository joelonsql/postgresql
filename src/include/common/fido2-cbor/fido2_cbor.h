/*-------------------------------------------------------------------------
 *
 * fido2_cbor.h
 *	  Minimal CBOR decoder for FIDO2 authenticator data parsing
 *
 * This is a standalone CBOR decoder designed specifically for parsing
 * FIDO2/WebAuthn authenticator data structures. It does not depend on
 * PostgreSQL's JSONB infrastructure.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/fido2-cbor/fido2_cbor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FIDO2_CBOR_H
#define FIDO2_CBOR_H

#include "c.h"
#include <stdint.h>
#include <stddef.h>

/*
 * CBOR Major Types (bits 7-5 of initial byte)
 */
#define CBOR_MAJOR_UNSIGNED		0	/* Unsigned integer */
#define CBOR_MAJOR_NEGATIVE		1	/* Negative integer */
#define CBOR_MAJOR_BYTE_STRING	2	/* Byte string */
#define CBOR_MAJOR_TEXT_STRING	3	/* Text string */
#define CBOR_MAJOR_ARRAY		4	/* Array */
#define CBOR_MAJOR_MAP			5	/* Map */
#define CBOR_MAJOR_TAG			6	/* Semantic tag */
#define CBOR_MAJOR_SIMPLE		7	/* Simple values and floats */

/*
 * CBOR Additional Type values (bits 4-0 of initial byte)
 */
#define CBOR_ADD_1BYTE			24	/* Following 1 byte contains value */
#define CBOR_ADD_2BYTE			25	/* Following 2 bytes contain value */
#define CBOR_ADD_4BYTE			26	/* Following 4 bytes contain value */
#define CBOR_ADD_8BYTE			27	/* Following 8 bytes contain value */
#define CBOR_ADD_INDEFINITE		31	/* Indefinite-length item */

/*
 * CBOR Simple Values
 */
#define CBOR_FALSE				20
#define CBOR_TRUE				21
#define CBOR_NULL				22
#define CBOR_UNDEFINED			23

/* Helper macros */
#define CBOR_GET_MAJOR(byte)	(((byte) >> 5) & 0x07)
#define CBOR_GET_ADDITIONAL(byte)	((byte) & 0x1F)

/*
 * CBOR value types for our decoder
 */
typedef enum CborValueType
{
	CBOR_TYPE_UINT,
	CBOR_TYPE_NEGINT,
	CBOR_TYPE_BYTES,
	CBOR_TYPE_TEXT,
	CBOR_TYPE_ARRAY,
	CBOR_TYPE_MAP,
	CBOR_TYPE_TAG,
	CBOR_TYPE_BOOL,
	CBOR_TYPE_NULL,
	CBOR_TYPE_UNDEFINED,
	CBOR_TYPE_FLOAT,
	CBOR_TYPE_INVALID
} CborValueType;

/*
 * Decoded CBOR value
 */
typedef struct CborValue
{
	CborValueType	type;
	union
	{
		uint64_t	uint_val;		/* For UINT */
		int64_t		int_val;		/* For NEGINT */
		struct
		{
			const uint8_t  *data;
			size_t			len;
		}			bytes;			/* For BYTES and TEXT */
		size_t		count;			/* For ARRAY and MAP (item count) */
		uint64_t	tag;			/* For TAG */
		bool		bool_val;		/* For BOOL */
		double		float_val;		/* For FLOAT */
	}			val;
} CborValue;

/*
 * CBOR decoder state
 */
typedef struct CborDecoder
{
	const uint8_t  *data;
	size_t			remaining;
	const char	   *error;
} CborDecoder;

/*
 * Initialize a CBOR decoder
 */
static inline void
cbor_decoder_init(CborDecoder *dec, const uint8_t *data, size_t len)
{
	dec->data = data;
	dec->remaining = len;
	dec->error = NULL;
}

/*
 * Function declarations
 */
extern bool cbor_decode_value(CborDecoder *dec, CborValue *val);
extern bool cbor_skip_value(CborDecoder *dec);
extern bool cbor_decode_uint(CborDecoder *dec, uint64_t *out);
extern bool cbor_decode_bytes(CborDecoder *dec, const uint8_t **out, size_t *len);
extern bool cbor_decode_text(CborDecoder *dec, const char **out, size_t *len);
extern bool cbor_decode_map_start(CborDecoder *dec, size_t *count);
extern bool cbor_decode_array_start(CborDecoder *dec, size_t *count);

/*
 * FIDO2-specific authenticator data parsing
 */

/* Authenticator data flags */
#define FIDO2_FLAG_UP		0x01	/* User Present */
#define FIDO2_FLAG_UV		0x04	/* User Verified */
#define FIDO2_FLAG_BE		0x08	/* Backup Eligibility */
#define FIDO2_FLAG_BS		0x10	/* Backup State */
#define FIDO2_FLAG_AT		0x40	/* Attested credential data included */
#define FIDO2_FLAG_ED		0x80	/* Extension data included */

/*
 * Parsed authenticator data
 */
typedef struct Fido2AuthData
{
	uint8_t			rp_id_hash[32];		/* SHA-256 hash of RP ID */
	uint8_t			flags;				/* Authenticator flags */
	uint32_t		sign_count;			/* Signature counter */

	/* Present only if AT flag is set */
	bool			has_attested_cred;
	uint8_t			aaguid[16];			/* Authenticator AAGUID */
	const uint8_t  *credential_id;		/* Credential ID */
	size_t			credential_id_len;
	const uint8_t  *public_key_cose;	/* COSE-encoded public key */
	size_t			public_key_cose_len;

	/* Present only if ED flag is set */
	bool			has_extensions;
	const uint8_t  *extensions;
	size_t			extensions_len;
} Fido2AuthData;

/*
 * Parse FIDO2 authenticator data
 * Returns true on success, false on error (check dec->error)
 */
extern bool fido2_parse_auth_data(const uint8_t *data, size_t len,
								  Fido2AuthData *auth_data, const char **error);

/*
 * Parse COSE public key and extract raw EC point (for ES256)
 * Returns true on success. On success, x and y each contain 32 bytes.
 */
extern bool fido2_parse_cose_es256_pubkey(const uint8_t *cose_key, size_t cose_len,
										  uint8_t *x, uint8_t *y, const char **error);

#endif							/* FIDO2_CBOR_H */
