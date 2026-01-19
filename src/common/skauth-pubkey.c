/*-------------------------------------------------------------------------
 *
 * skauth-pubkey.c
 *	  OpenSSH sk-ecdsa public key parser for sk-provider authentication
 *
 * This module parses OpenSSH sk-ecdsa-sha2-nistp256@openssh.com public keys
 * and extracts the EC point and credential information needed for sk-provider
 * authentication.
 *
 * OpenSSH sk-ecdsa public key format (after base64 decoding):
 *   string    key type ("sk-ecdsa-sha2-nistp256@openssh.com")
 *   string    curve name ("nistp256")
 *   string    EC point (65 bytes: 0x04 || x[32] || y[32])
 *   string    application (e.g., "ssh:")
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/skauth-pubkey.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/base64.h"
#include "common/skauth-pubkey.h"

#include <string.h>

/* Expected key type for sk-ecdsa */
#define SK_ECDSA_KEY_TYPE	"sk-ecdsa-sha2-nistp256@openssh.com"
#define SK_ECDSA_CURVE		"nistp256"

/* EC point size for P-256 (uncompressed: 0x04 + 32 + 32) */
#define EC_POINT_SIZE		65

/*
 * Read a 32-bit big-endian length from buffer
 */
static uint32_t
read_uint32_be(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) |
		   ((uint32_t) p[1] << 16) |
		   ((uint32_t) p[2] << 8) |
		   ((uint32_t) p[3]);
}

/*
 * Read an OpenSSH string (length-prefixed) from buffer
 *
 * Returns pointer to string data (not null-terminated) and updates
 * *len with the string length. Advances *pp past the string.
 * Returns NULL if buffer too small.
 */
static const uint8_t *
read_ssh_string(const uint8_t **pp, const uint8_t *end, uint32_t *len)
{
	const uint8_t *p = *pp;
	const uint8_t *data;

	if (end - p < 4)
		return NULL;

	*len = read_uint32_be(p);
	p += 4;

	if (end - p < *len)
		return NULL;

	data = p;
	*pp = p + *len;

	return data;
}

/*
 * Parse an OpenSSH sk-ecdsa public key string
 *
 * Input format: "sk-ecdsa-sha2-nistp256@openssh.com AAAA... [comment]"
 *
 * On success, fills in the SkauthParsedPubkey structure and returns true.
 * On failure, sets *errmsg and returns false.
 *
 * The caller must free the allocated fields in the structure when done.
 */
bool
skauth_parse_openssh_pubkey(const char *pubkey_str,
							SkauthParsedPubkey *result,
							char **errmsg)
{
	const char *key_data_start;
	const char *key_data_end;
	char	   *base64_data = NULL;
	uint8_t	   *decoded = NULL;
	int			decoded_len;
	const uint8_t *p;
	const uint8_t *end;
	const uint8_t *str_data;
	uint32_t	str_len;

	*errmsg = NULL;
	memset(result, 0, sizeof(SkauthParsedPubkey));

	/* Skip leading whitespace */
	while (*pubkey_str && (*pubkey_str == ' ' || *pubkey_str == '\t'))
		pubkey_str++;

	/* Check for key type prefix */
	if (strncmp(pubkey_str, SK_ECDSA_KEY_TYPE, strlen(SK_ECDSA_KEY_TYPE)) != 0)
	{
		*errmsg = strdup("key type must be sk-ecdsa-sha2-nistp256@openssh.com");
		return false;
	}

	pubkey_str += strlen(SK_ECDSA_KEY_TYPE);

	/* Skip whitespace between key type and base64 data */
	while (*pubkey_str && (*pubkey_str == ' ' || *pubkey_str == '\t'))
		pubkey_str++;

	if (*pubkey_str == '\0')
	{
		*errmsg = strdup("missing key data after key type");
		return false;
	}

	/* Find end of base64 data (space or end of string) */
	key_data_start = pubkey_str;
	key_data_end = key_data_start;
	while (*key_data_end && *key_data_end != ' ' && *key_data_end != '\t' &&
		   *key_data_end != '\n' && *key_data_end != '\r')
		key_data_end++;

	/* Copy base64 data for decoding */
	base64_data = malloc(key_data_end - key_data_start + 1);
	if (!base64_data)
	{
		*errmsg = strdup("out of memory");
		return false;
	}
	memcpy(base64_data, key_data_start, key_data_end - key_data_start);
	base64_data[key_data_end - key_data_start] = '\0';

	/* Decode base64 */
	decoded_len = pg_b64_dec_len(strlen(base64_data));
	decoded = malloc(decoded_len);
	if (!decoded)
	{
		free(base64_data);
		*errmsg = strdup("out of memory");
		return false;
	}

	decoded_len = pg_b64_decode(base64_data, strlen(base64_data),
							   decoded, decoded_len);
	free(base64_data);

	if (decoded_len < 0)
	{
		free(decoded);
		*errmsg = strdup("invalid base64 encoding in public key");
		return false;
	}

	/* Parse the decoded key structure */
	p = decoded;
	end = decoded + decoded_len;

	/* Read key type string */
	str_data = read_ssh_string(&p, end, &str_len);
	if (!str_data)
	{
		free(decoded);
		*errmsg = strdup("truncated key data: missing key type");
		return false;
	}

	if (str_len != strlen(SK_ECDSA_KEY_TYPE) ||
		memcmp(str_data, SK_ECDSA_KEY_TYPE, str_len) != 0)
	{
		free(decoded);
		*errmsg = strdup("key type mismatch in encoded data");
		return false;
	}

	/* Read curve name */
	str_data = read_ssh_string(&p, end, &str_len);
	if (!str_data)
	{
		free(decoded);
		*errmsg = strdup("truncated key data: missing curve name");
		return false;
	}

	if (str_len != strlen(SK_ECDSA_CURVE) ||
		memcmp(str_data, SK_ECDSA_CURVE, str_len) != 0)
	{
		free(decoded);
		*errmsg = strdup("unsupported curve: only nistp256 is supported");
		return false;
	}

	/* Read EC point */
	str_data = read_ssh_string(&p, end, &str_len);
	if (!str_data)
	{
		free(decoded);
		*errmsg = strdup("truncated key data: missing EC point");
		return false;
	}

	if (str_len != EC_POINT_SIZE)
	{
		free(decoded);
		*errmsg = strdup("invalid EC point size: expected 65 bytes");
		return false;
	}

	if (str_data[0] != 0x04)
	{
		free(decoded);
		*errmsg = strdup("invalid EC point: must be uncompressed (0x04 prefix)");
		return false;
	}

	/* Copy EC point */
	result->public_key = malloc(EC_POINT_SIZE);
	if (!result->public_key)
	{
		free(decoded);
		*errmsg = strdup("out of memory");
		return false;
	}
	memcpy(result->public_key, str_data, EC_POINT_SIZE);
	result->public_key_len = EC_POINT_SIZE;

	/* Read application string */
	str_data = read_ssh_string(&p, end, &str_len);
	if (!str_data)
	{
		free(result->public_key);
		result->public_key = NULL;
		free(decoded);
		*errmsg = strdup("truncated key data: missing application");
		return false;
	}

	/* Copy application (as null-terminated string) */
	result->application = malloc(str_len + 1);
	if (!result->application)
	{
		free(result->public_key);
		result->public_key = NULL;
		free(decoded);
		*errmsg = strdup("out of memory");
		return false;
	}
	memcpy(result->application, str_data, str_len);
	result->application[str_len] = '\0';

	/*
	 * Note: OpenSSH sk-ecdsa keys may have additional data (flags, etc.)
	 * after the application. For now, we ignore any extra data.
	 */

	/* Set algorithm identifier */
	result->algorithm = SKAUTH_ALG_ES256;

	free(decoded);
	return true;
}

/*
 * Free a parsed public key structure
 */
void
skauth_free_parsed_pubkey(SkauthParsedPubkey *pubkey)
{
	if (pubkey)
	{
		if (pubkey->public_key)
		{
			free(pubkey->public_key);
			pubkey->public_key = NULL;
		}
		if (pubkey->application)
		{
			free(pubkey->application);
			pubkey->application = NULL;
		}
	}
}
