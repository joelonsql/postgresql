/*-------------------------------------------------------------------------
 *
 * fido2_cbor.c
 *	  Minimal CBOR decoder for FIDO2 authenticator data parsing
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/fido2-cbor/fido2_cbor.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include "common/fido2-cbor/fido2_cbor.h"

#include <string.h>

/*
 * Read big-endian integer from buffer
 */
static inline uint16_t
read_be16(const uint8_t *p)
{
	return ((uint16_t) p[0] << 8) | p[1];
}

static inline uint32_t
read_be32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
		   ((uint32_t) p[2] << 8) | p[3];
}

static inline uint64_t
read_be64(const uint8_t *p)
{
	return ((uint64_t) p[0] << 56) | ((uint64_t) p[1] << 48) |
		   ((uint64_t) p[2] << 40) | ((uint64_t) p[3] << 32) |
		   ((uint64_t) p[4] << 24) | ((uint64_t) p[5] << 16) |
		   ((uint64_t) p[6] << 8) | p[7];
}

/*
 * Decode a CBOR value's header and get the argument value
 * Returns false on error
 */
static bool
cbor_decode_header(CborDecoder *dec, uint8_t *major_out, uint64_t *arg_out)
{
	uint8_t		initial;
	uint8_t		major;
	uint8_t		additional;
	uint64_t	arg;

	if (dec->remaining < 1)
	{
		dec->error = "unexpected end of CBOR data";
		return false;
	}

	initial = *dec->data++;
	dec->remaining--;

	major = CBOR_GET_MAJOR(initial);
	additional = CBOR_GET_ADDITIONAL(initial);

	if (additional < 24)
	{
		arg = additional;
	}
	else if (additional == CBOR_ADD_1BYTE)
	{
		if (dec->remaining < 1)
		{
			dec->error = "unexpected end of CBOR data";
			return false;
		}
		arg = *dec->data++;
		dec->remaining--;
	}
	else if (additional == CBOR_ADD_2BYTE)
	{
		if (dec->remaining < 2)
		{
			dec->error = "unexpected end of CBOR data";
			return false;
		}
		arg = read_be16(dec->data);
		dec->data += 2;
		dec->remaining -= 2;
	}
	else if (additional == CBOR_ADD_4BYTE)
	{
		if (dec->remaining < 4)
		{
			dec->error = "unexpected end of CBOR data";
			return false;
		}
		arg = read_be32(dec->data);
		dec->data += 4;
		dec->remaining -= 4;
	}
	else if (additional == CBOR_ADD_8BYTE)
	{
		if (dec->remaining < 8)
		{
			dec->error = "unexpected end of CBOR data";
			return false;
		}
		arg = read_be64(dec->data);
		dec->data += 8;
		dec->remaining -= 8;
	}
	else if (additional == CBOR_ADD_INDEFINITE)
	{
		/* Indefinite length - set arg to special marker */
		arg = UINT64_MAX;
	}
	else
	{
		dec->error = "invalid CBOR additional info";
		return false;
	}

	*major_out = major;
	*arg_out = arg;
	return true;
}

/*
 * Decode a CBOR value
 */
bool
cbor_decode_value(CborDecoder *dec, CborValue *val)
{
	uint8_t		major;
	uint64_t	arg;

	if (!cbor_decode_header(dec, &major, &arg))
		return false;

	switch (major)
	{
		case CBOR_MAJOR_UNSIGNED:
			val->type = CBOR_TYPE_UINT;
			val->val.uint_val = arg;
			break;

		case CBOR_MAJOR_NEGATIVE:
			val->type = CBOR_TYPE_NEGINT;
			/* CBOR negative is -1 - arg */
			val->val.int_val = -1 - (int64_t) arg;
			break;

		case CBOR_MAJOR_BYTE_STRING:
			if (arg == UINT64_MAX)
			{
				dec->error = "indefinite byte strings not supported";
				return false;
			}
			if (arg > dec->remaining)
			{
				dec->error = "byte string length exceeds available data";
				return false;
			}
			val->type = CBOR_TYPE_BYTES;
			val->val.bytes.data = dec->data;
			val->val.bytes.len = (size_t) arg;
			dec->data += arg;
			dec->remaining -= arg;
			break;

		case CBOR_MAJOR_TEXT_STRING:
			if (arg == UINT64_MAX)
			{
				dec->error = "indefinite text strings not supported";
				return false;
			}
			if (arg > dec->remaining)
			{
				dec->error = "text string length exceeds available data";
				return false;
			}
			val->type = CBOR_TYPE_TEXT;
			val->val.bytes.data = dec->data;
			val->val.bytes.len = (size_t) arg;
			dec->data += arg;
			dec->remaining -= arg;
			break;

		case CBOR_MAJOR_ARRAY:
			if (arg == UINT64_MAX)
			{
				dec->error = "indefinite arrays not supported";
				return false;
			}
			val->type = CBOR_TYPE_ARRAY;
			val->val.count = (size_t) arg;
			break;

		case CBOR_MAJOR_MAP:
			if (arg == UINT64_MAX)
			{
				dec->error = "indefinite maps not supported";
				return false;
			}
			val->type = CBOR_TYPE_MAP;
			val->val.count = (size_t) arg;
			break;

		case CBOR_MAJOR_TAG:
			val->type = CBOR_TYPE_TAG;
			val->val.tag = arg;
			break;

		case CBOR_MAJOR_SIMPLE:
			if (arg == CBOR_FALSE)
			{
				val->type = CBOR_TYPE_BOOL;
				val->val.bool_val = false;
			}
			else if (arg == CBOR_TRUE)
			{
				val->type = CBOR_TYPE_BOOL;
				val->val.bool_val = true;
			}
			else if (arg == CBOR_NULL)
			{
				val->type = CBOR_TYPE_NULL;
			}
			else if (arg == CBOR_UNDEFINED)
			{
				val->type = CBOR_TYPE_UNDEFINED;
			}
			else
			{
				/* Could be float - not needed for FIDO2 */
				dec->error = "unsupported simple/float type";
				return false;
			}
			break;

		default:
			dec->error = "unknown CBOR major type";
			return false;
	}

	return true;
}

/*
 * Skip over a CBOR value without fully decoding it
 */
bool
cbor_skip_value(CborDecoder *dec)
{
	CborValue	val;
	size_t		i;

	if (!cbor_decode_value(dec, &val))
		return false;

	switch (val.type)
	{
		case CBOR_TYPE_ARRAY:
			for (i = 0; i < val.val.count; i++)
			{
				if (!cbor_skip_value(dec))
					return false;
			}
			break;

		case CBOR_TYPE_MAP:
			for (i = 0; i < val.val.count; i++)
			{
				/* Skip key and value */
				if (!cbor_skip_value(dec))
					return false;
				if (!cbor_skip_value(dec))
					return false;
			}
			break;

		case CBOR_TYPE_TAG:
			/* Skip the tagged value */
			if (!cbor_skip_value(dec))
				return false;
			break;

		default:
			/* Other types are already consumed */
			break;
	}

	return true;
}

/*
 * Decode an unsigned integer
 */
bool
cbor_decode_uint(CborDecoder *dec, uint64_t *out)
{
	CborValue	val;

	if (!cbor_decode_value(dec, &val))
		return false;

	if (val.type != CBOR_TYPE_UINT)
	{
		dec->error = "expected unsigned integer";
		return false;
	}

	*out = val.val.uint_val;
	return true;
}

/*
 * Decode a byte string
 */
bool
cbor_decode_bytes(CborDecoder *dec, const uint8_t **out, size_t *len)
{
	CborValue	val;

	if (!cbor_decode_value(dec, &val))
		return false;

	if (val.type != CBOR_TYPE_BYTES)
	{
		dec->error = "expected byte string";
		return false;
	}

	*out = val.val.bytes.data;
	*len = val.val.bytes.len;
	return true;
}

/*
 * Decode a text string
 */
bool
cbor_decode_text(CborDecoder *dec, const char **out, size_t *len)
{
	CborValue	val;

	if (!cbor_decode_value(dec, &val))
		return false;

	if (val.type != CBOR_TYPE_TEXT)
	{
		dec->error = "expected text string";
		return false;
	}

	*out = (const char *) val.val.bytes.data;
	*len = val.val.bytes.len;
	return true;
}

/*
 * Decode the start of a map and get item count
 */
bool
cbor_decode_map_start(CborDecoder *dec, size_t *count)
{
	CborValue	val;

	if (!cbor_decode_value(dec, &val))
		return false;

	if (val.type != CBOR_TYPE_MAP)
	{
		dec->error = "expected map";
		return false;
	}

	*count = val.val.count;
	return true;
}

/*
 * Decode the start of an array and get item count
 */
bool
cbor_decode_array_start(CborDecoder *dec, size_t *count)
{
	CborValue	val;

	if (!cbor_decode_value(dec, &val))
		return false;

	if (val.type != CBOR_TYPE_ARRAY)
	{
		dec->error = "expected array";
		return false;
	}

	*count = val.val.count;
	return true;
}

/*
 * Parse FIDO2 authenticator data
 *
 * Authenticator data format (from WebAuthn spec):
 *   - rpIdHash: 32 bytes
 *   - flags: 1 byte
 *   - signCount: 4 bytes (big-endian)
 *   - attestedCredentialData (if AT flag set):
 *       - aaguid: 16 bytes
 *       - credentialIdLength: 2 bytes (big-endian)
 *       - credentialId: credentialIdLength bytes
 *       - credentialPublicKey: COSE_Key format (CBOR)
 *   - extensions (if ED flag set): CBOR map
 */
bool
fido2_parse_auth_data(const uint8_t *data, size_t len,
					  Fido2AuthData *auth_data, const char **error)
{
	const uint8_t  *p = data;
	size_t			remaining = len;
	uint16_t		cred_id_len;

	memset(auth_data, 0, sizeof(Fido2AuthData));

	/* Minimum size: rpIdHash(32) + flags(1) + signCount(4) = 37 bytes */
	if (remaining < 37)
	{
		*error = "authenticator data too short";
		return false;
	}

	/* rpIdHash: 32 bytes */
	memcpy(auth_data->rp_id_hash, p, 32);
	p += 32;
	remaining -= 32;

	/* flags: 1 byte */
	auth_data->flags = *p++;
	remaining--;

	/* signCount: 4 bytes big-endian */
	auth_data->sign_count = read_be32(p);
	p += 4;
	remaining -= 4;

	/* Check for attested credential data */
	if (auth_data->flags & FIDO2_FLAG_AT)
	{
		CborDecoder dec;
		const uint8_t *cose_start;

		auth_data->has_attested_cred = true;

		/* Need at least aaguid(16) + credIdLen(2) */
		if (remaining < 18)
		{
			*error = "attested credential data too short";
			return false;
		}

		/* aaguid: 16 bytes */
		memcpy(auth_data->aaguid, p, 16);
		p += 16;
		remaining -= 16;

		/* credentialIdLength: 2 bytes big-endian */
		cred_id_len = read_be16(p);
		p += 2;
		remaining -= 2;

		if (remaining < cred_id_len)
		{
			*error = "credential ID length exceeds available data";
			return false;
		}

		/* credentialId */
		auth_data->credential_id = p;
		auth_data->credential_id_len = cred_id_len;
		p += cred_id_len;
		remaining -= cred_id_len;

		/* credentialPublicKey: COSE_Key in CBOR format */
		/* We need to find where the COSE key ends by parsing the CBOR */
		cose_start = p;
		cbor_decoder_init(&dec, p, remaining);

		if (!cbor_skip_value(&dec))
		{
			*error = dec.error ? dec.error : "failed to parse COSE public key";
			return false;
		}

		auth_data->public_key_cose = cose_start;
		auth_data->public_key_cose_len = p + remaining - dec.data - dec.remaining;
		p = dec.data;
		remaining = dec.remaining;
	}

	/* Check for extension data */
	if (auth_data->flags & FIDO2_FLAG_ED)
	{
		CborDecoder dec;

		auth_data->has_extensions = true;
		auth_data->extensions = p;

		/* Parse to find the end of extensions CBOR */
		cbor_decoder_init(&dec, p, remaining);

		if (!cbor_skip_value(&dec))
		{
			*error = dec.error ? dec.error : "failed to parse extensions";
			return false;
		}

		auth_data->extensions_len = p + remaining - dec.data - dec.remaining;
	}

	return true;
}

/*
 * COSE key labels for EC2 keys (ES256)
 */
#define COSE_KEY_KTY		1	/* Key type */
#define COSE_KEY_ALG		3	/* Algorithm */
#define COSE_KEY_CRV		-1	/* Curve (EC2) */
#define COSE_KEY_X			-2	/* X coordinate (EC2) */
#define COSE_KEY_Y			-3	/* Y coordinate (EC2) */

#define COSE_KTY_EC2		2	/* EC2 key type */
#define COSE_ALG_ES256		-7	/* ES256 algorithm */
#define COSE_CRV_P256		1	/* P-256 curve */

/*
 * Parse a COSE ES256 public key and extract the raw EC point
 *
 * COSE_Key for ES256 is a CBOR map with:
 *   1 (kty): 2 (EC2)
 *   3 (alg): -7 (ES256)
 *   -1 (crv): 1 (P-256)
 *   -2 (x): bytes (32 bytes)
 *   -3 (y): bytes (32 bytes)
 */
bool
fido2_parse_cose_es256_pubkey(const uint8_t *cose_key, size_t cose_len,
							  uint8_t *x, uint8_t *y, const char **error)
{
	CborDecoder	dec;
	size_t		map_count;
	size_t		i;
	bool		have_x = false;
	bool		have_y = false;
	bool		have_kty = false;
	bool		have_alg = false;
	bool		have_crv = false;

	cbor_decoder_init(&dec, cose_key, cose_len);

	if (!cbor_decode_map_start(&dec, &map_count))
	{
		*error = dec.error ? dec.error : "COSE key is not a map";
		return false;
	}

	for (i = 0; i < map_count; i++)
	{
		CborValue	key_val;
		int64_t		label;

		/* Decode the key (should be an integer label) */
		if (!cbor_decode_value(&dec, &key_val))
		{
			*error = dec.error ? dec.error : "failed to decode COSE key label";
			return false;
		}

		if (key_val.type == CBOR_TYPE_UINT)
			label = (int64_t) key_val.val.uint_val;
		else if (key_val.type == CBOR_TYPE_NEGINT)
			label = key_val.val.int_val;
		else
		{
			/* Skip unknown key type */
			if (!cbor_skip_value(&dec))
			{
				*error = dec.error;
				return false;
			}
			continue;
		}

		switch (label)
		{
			case COSE_KEY_KTY:
				{
					uint64_t	kty;

					if (!cbor_decode_uint(&dec, &kty))
					{
						*error = dec.error ? dec.error : "invalid kty";
						return false;
					}
					if (kty != COSE_KTY_EC2)
					{
						*error = "unsupported key type (not EC2)";
						return false;
					}
					have_kty = true;
				}
				break;

			case COSE_KEY_ALG:
				{
					CborValue	alg_val;

					if (!cbor_decode_value(&dec, &alg_val))
					{
						*error = dec.error ? dec.error : "invalid alg";
						return false;
					}
					if (alg_val.type == CBOR_TYPE_NEGINT)
					{
						if (alg_val.val.int_val != COSE_ALG_ES256)
						{
							*error = "unsupported algorithm (not ES256)";
							return false;
						}
					}
					else
					{
						*error = "algorithm must be negative integer";
						return false;
					}
					have_alg = true;
				}
				break;

			case COSE_KEY_CRV:
				{
					uint64_t	crv;

					if (!cbor_decode_uint(&dec, &crv))
					{
						*error = dec.error ? dec.error : "invalid crv";
						return false;
					}
					if (crv != COSE_CRV_P256)
					{
						*error = "unsupported curve (not P-256)";
						return false;
					}
					have_crv = true;
				}
				break;

			case COSE_KEY_X:
				{
					const uint8_t  *xdata;
					size_t			xlen;

					if (!cbor_decode_bytes(&dec, &xdata, &xlen))
					{
						*error = dec.error ? dec.error : "invalid x coordinate";
						return false;
					}
					if (xlen != 32)
					{
						*error = "x coordinate must be 32 bytes";
						return false;
					}
					memcpy(x, xdata, 32);
					have_x = true;
				}
				break;

			case COSE_KEY_Y:
				{
					const uint8_t  *ydata;
					size_t			ylen;

					if (!cbor_decode_bytes(&dec, &ydata, &ylen))
					{
						*error = dec.error ? dec.error : "invalid y coordinate";
						return false;
					}
					if (ylen != 32)
					{
						*error = "y coordinate must be 32 bytes";
						return false;
					}
					memcpy(y, ydata, 32);
					have_y = true;
				}
				break;

			default:
				/* Skip unknown label */
				if (!cbor_skip_value(&dec))
				{
					*error = dec.error;
					return false;
				}
				break;
		}
	}

	if (!have_kty || !have_alg || !have_crv || !have_x || !have_y)
	{
		*error = "COSE key missing required fields";
		return false;
	}

	return true;
}
