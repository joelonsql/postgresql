/*-------------------------------------------------------------------------
 * skauth-pubkey.c
 *	  OpenSSH sk-ecdsa public key parser
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/skauth-pubkey.c
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/base64.h"
#include "libpq/skauth.h"

#include <string.h>

#define SK_KEY_TYPE		"sk-ecdsa-sha2-nistp256@openssh.com"
#define SK_CURVE		"nistp256"

bool
skauth_parse_openssh_pubkey(const char *pubkey_str, SkauthParsedPubkey *result, char **errmsg)
{
	const char *p, *end;
	char	   *b64 = NULL;
	uint8_t	   *dec = NULL;
	int			dec_len;
	const uint8_t *dp, *dend;
	uint32_t	len;

	*errmsg = NULL;
	memset(result, 0, sizeof(SkauthParsedPubkey));

	/* Skip whitespace and check key type */
	while (*pubkey_str == ' ' || *pubkey_str == '\t')
		pubkey_str++;

	if (strncmp(pubkey_str, SK_KEY_TYPE, strlen(SK_KEY_TYPE)) != 0)
	{
		*errmsg = strdup("key type must be sk-ecdsa-sha2-nistp256@openssh.com");
		return false;
	}
	pubkey_str += strlen(SK_KEY_TYPE);

	while (*pubkey_str == ' ' || *pubkey_str == '\t')
		pubkey_str++;
	if (!*pubkey_str)
	{
		*errmsg = strdup("missing key data");
		return false;
	}

	/* Find base64 data end */
	p = pubkey_str;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
		p++;
	end = p;

	/* Decode base64 */
	b64 = malloc(end - pubkey_str + 1);
	if (!b64)
		goto oom;
	memcpy(b64, pubkey_str, end - pubkey_str);
	b64[end - pubkey_str] = '\0';

	dec_len = pg_b64_dec_len(strlen(b64));
	dec = malloc(dec_len);
	if (!dec)
		goto oom;

	dec_len = pg_b64_decode(b64, strlen(b64), dec, dec_len);
	free(b64);
	b64 = NULL;

	if (dec_len < 0)
	{
		*errmsg = strdup("invalid base64");
		goto fail;
	}

	dp = dec;
	dend = dec + dec_len;

#define READ_STRING(ptr, slen) do { \
	if (dend - dp < 4) goto trunc; \
	slen = ((uint32_t)dp[0] << 24) | ((uint32_t)dp[1] << 16) | ((uint32_t)dp[2] << 8) | dp[3]; \
	dp += 4; \
	if (dend - dp < slen) goto trunc; \
	ptr = dp; \
	dp += slen; \
} while(0)

	/* Read and verify key type */
	{
		const uint8_t *s;
		uint32_t slen;
		READ_STRING(s, slen);
		if (slen != strlen(SK_KEY_TYPE) || memcmp(s, SK_KEY_TYPE, slen) != 0)
		{
			*errmsg = strdup("key type mismatch");
			goto fail;
		}
	}

	/* Read and verify curve */
	{
		const uint8_t *s;
		uint32_t slen;
		READ_STRING(s, slen);
		if (slen != strlen(SK_CURVE) || memcmp(s, SK_CURVE, slen) != 0)
		{
			*errmsg = strdup("unsupported curve");
			goto fail;
		}
	}

	/* Read EC point */
	{
		const uint8_t *s;
		uint32_t slen;
		READ_STRING(s, slen);
		if (slen != 65 || s[0] != 0x04)
		{
			*errmsg = strdup("invalid EC point");
			goto fail;
		}
		result->public_key = malloc(65);
		if (!result->public_key)
			goto oom;
		memcpy(result->public_key, s, 65);
		result->public_key_len = 65;
	}

	/* Read application */
	{
		const uint8_t *s;
		uint32_t slen;
		READ_STRING(s, slen);
		result->application = malloc(slen + 1);
		if (!result->application)
			goto oom;
		memcpy(result->application, s, slen);
		result->application[slen] = '\0';
	}

#undef READ_STRING

	result->algorithm = COSE_ALG_ES256;
	free(dec);
	return true;

trunc:
	*errmsg = strdup("truncated key data");
	goto fail;

oom:
	*errmsg = strdup("out of memory");

fail:
	free(b64);
	free(dec);
	free(result->public_key);
	free(result->application);
	result->public_key = NULL;
	result->application = NULL;
	return false;
}

void
skauth_free_parsed_pubkey(SkauthParsedPubkey *pubkey)
{
	if (pubkey)
	{
		free(pubkey->public_key);
		free(pubkey->application);
		pubkey->public_key = NULL;
		pubkey->application = NULL;
	}
}
