/*-------------------------------------------------------------------------
 * fido2-x509.c
 *	  X.509 certificate handling for FIDO2 TLS authentication
 *
 * This module provides functions to:
 * - Build X.509 certificates with FIDO2 assertion extensions (client)
 * - Parse FIDO2 assertion extensions from received certificates (server)
 *
 * The FIDO2 assertion is carried in a custom X.509 extension with OID
 * 1.3.6.1.4.1.58324.1.1 (using a private enterprise number for PostgreSQL).
 *
 * Extension format (DER SEQUENCE):
 *   - flags      OCTET STRING (1 byte)
 *   - counter    INTEGER (4 bytes)
 *   - signature  OCTET STRING (64 bytes, r||s)
 *   - challenge  OCTET STRING (32 bytes)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/fido2-x509.c
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "libpq/fido2.h"

#ifdef USE_OPENSSL
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/asn1.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <string.h>
#include <time.h>

/*
 * Custom OID for FIDO2 assertion extension.
 * Using 1.3.6.1.4.1.58324.1.1:
 *   1.3.6.1.4.1 = ISO assigned OIDs, private enterprise
 *   58324 = PostgreSQL IANA private enterprise number
 *   1.1 = FIDO2 assertion extension
 */
#define FIDO2_EXTENSION_OID		"1.3.6.1.4.1.58324.1.1"

/*
 * Create an EC public key from raw uncompressed point data.
 * The pubkey_raw must be 65 bytes: 0x04 || X(32) || Y(32)
 */
EVP_PKEY *
fido2_x509_create_ec_pkey(const uint8_t *pubkey_raw)
{
	EVP_PKEY   *pkey = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	OSSL_PARAM_BLD *param_bld = NULL;
	OSSL_PARAM *params = NULL;

	if (!pubkey_raw || pubkey_raw[0] != 0x04)
		return NULL;

	param_bld = OSSL_PARAM_BLD_new();
	if (!param_bld)
		return NULL;

	/* Build parameters for EC key */
	if (!OSSL_PARAM_BLD_push_utf8_string(param_bld, "group", "prime256v1", 0) ||
		!OSSL_PARAM_BLD_push_octet_string(param_bld, "pub", pubkey_raw, 65))
	{
		OSSL_PARAM_BLD_free(param_bld);
		return NULL;
	}

	params = OSSL_PARAM_BLD_to_param(param_bld);
	OSSL_PARAM_BLD_free(param_bld);

	if (!params)
		return NULL;

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	if (!ctx)
	{
		OSSL_PARAM_free(params);
		return NULL;
	}

	if (EVP_PKEY_fromdata_init(ctx) <= 0 ||
		EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		OSSL_PARAM_free(params);
		return NULL;
	}

	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);

	return pkey;
}

/*
 * Create an ephemeral EC key pair for self-signing the X.509 certificate.
 * This is NOT the FIDO2 key - it's just used for the X.509 self-signature.
 */
static EVP_PKEY *
fido2_x509_create_ephemeral_key(void)
{
	EVP_PKEY   *pkey = NULL;
	EVP_PKEY_CTX *ctx;

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	if (!ctx)
		return NULL;

	if (EVP_PKEY_keygen_init(ctx) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return NULL;
	}

	if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return NULL;
	}

	if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return NULL;
	}

	EVP_PKEY_CTX_free(ctx);
	return pkey;
}

/*
 * Encode FIDO2 assertion data as a DER SEQUENCE for the X.509 extension.
 *
 * Format:
 *   SEQUENCE {
 *     pubkey     OCTET STRING (65 bytes, uncompressed EC point)
 *     flags      OCTET STRING (1 byte)
 *     counter    INTEGER
 *     signature  OCTET STRING (64 bytes)
 *     challenge  OCTET STRING (32 bytes)
 *   }
 *
 * The FIDO2 public key is included in the extension because the certificate's
 * Subject Public Key field contains an ephemeral key used for TLS
 * CertificateVerify signing.
 *
 * Returns the DER-encoded data and sets *len to the length.
 * Caller must free the returned buffer.
 */
static uint8_t *
fido2_x509_encode_assertion(const uint8_t *pubkey,
							uint8_t flags, uint32_t counter,
							const uint8_t *signature,
							const uint8_t *challenge,
							int *len)
{
	uint8_t	   *result = NULL;
	uint8_t	   *p;
	ASN1_OCTET_STRING *pubkey_oct = NULL;
	ASN1_OCTET_STRING *flags_oct = NULL;
	ASN1_OCTET_STRING *sig_oct = NULL;
	ASN1_OCTET_STRING *challenge_oct = NULL;
	ASN1_INTEGER *counter_int = NULL;
	int			pubkey_len, flags_len, counter_len, sig_len, challenge_len;
	int			total_len;
	int			seq_len;

	*len = 0;

	/* Create ASN.1 objects */
	pubkey_oct = ASN1_OCTET_STRING_new();
	if (!pubkey_oct || !ASN1_OCTET_STRING_set(pubkey_oct, pubkey, FIDO2_ES256_PUBKEY_LENGTH))
		goto cleanup;

	flags_oct = ASN1_OCTET_STRING_new();
	if (!flags_oct || !ASN1_OCTET_STRING_set(flags_oct, &flags, 1))
		goto cleanup;

	counter_int = ASN1_INTEGER_new();
	if (!counter_int || !ASN1_INTEGER_set(counter_int, counter))
		goto cleanup;

	sig_oct = ASN1_OCTET_STRING_new();
	if (!sig_oct || !ASN1_OCTET_STRING_set(sig_oct, signature, FIDO2_ES256_SIG_LENGTH))
		goto cleanup;

	challenge_oct = ASN1_OCTET_STRING_new();
	if (!challenge_oct || !ASN1_OCTET_STRING_set(challenge_oct, challenge, FIDO2_CHALLENGE_LENGTH))
		goto cleanup;

	/* Calculate encoded lengths */
	pubkey_len = i2d_ASN1_OCTET_STRING(pubkey_oct, NULL);
	flags_len = i2d_ASN1_OCTET_STRING(flags_oct, NULL);
	counter_len = i2d_ASN1_INTEGER(counter_int, NULL);
	sig_len = i2d_ASN1_OCTET_STRING(sig_oct, NULL);
	challenge_len = i2d_ASN1_OCTET_STRING(challenge_oct, NULL);

	if (pubkey_len < 0 || flags_len < 0 || counter_len < 0 || sig_len < 0 || challenge_len < 0)
		goto cleanup;

	seq_len = pubkey_len + flags_len + counter_len + sig_len + challenge_len;

	/* Calculate total length including SEQUENCE header */
	if (seq_len < 128)
		total_len = 2 + seq_len;	/* 30 LL ... */
	else if (seq_len < 256)
		total_len = 3 + seq_len;	/* 30 81 LL ... */
	else
		total_len = 4 + seq_len;	/* 30 82 LL LL ... */

	result = malloc(total_len);
	if (!result)
		goto cleanup;

	p = result;

	/* Write SEQUENCE header */
	*p++ = 0x30;					/* SEQUENCE tag */
	if (seq_len < 128)
	{
		*p++ = (uint8_t) seq_len;
	}
	else if (seq_len < 256)
	{
		*p++ = 0x81;
		*p++ = (uint8_t) seq_len;
	}
	else
	{
		*p++ = 0x82;
		*p++ = (uint8_t) (seq_len >> 8);
		*p++ = (uint8_t) seq_len;
	}

	/* Write contents */
	i2d_ASN1_OCTET_STRING(pubkey_oct, &p);
	i2d_ASN1_OCTET_STRING(flags_oct, &p);
	i2d_ASN1_INTEGER(counter_int, &p);
	i2d_ASN1_OCTET_STRING(sig_oct, &p);
	i2d_ASN1_OCTET_STRING(challenge_oct, &p);

	*len = total_len;

cleanup:
	ASN1_OCTET_STRING_free(pubkey_oct);
	ASN1_OCTET_STRING_free(flags_oct);
	ASN1_INTEGER_free(counter_int);
	ASN1_OCTET_STRING_free(sig_oct);
	ASN1_OCTET_STRING_free(challenge_oct);

	return result;
}

/*
 * Build an X.509 certificate containing the FIDO2 assertion.
 *
 * The certificate uses an ephemeral key pair for TLS CertificateVerify signing.
 * The FIDO2 public key is included in the extension, not in the certificate's
 * Subject Public Key field.
 *
 * Parameters:
 *   pubkey_raw  - 65-byte uncompressed FIDO2 public key (0x04 || X || Y)
 *   flags       - FIDO2 authenticator flags
 *   counter     - FIDO2 signature counter
 *   signature   - 64-byte FIDO2 signature (r || s)
 *   challenge   - 32-byte challenge that was signed
 *   cert_out    - Output: X509 certificate (caller must free with X509_free)
 *   pkey_out    - Output: ephemeral private key for TLS (caller must free with EVP_PKEY_free)
 *
 * Returns true on success, false on failure.
 */
bool
fido2_x509_build_cert(const uint8_t *pubkey_raw,
					  uint8_t flags, uint32_t counter,
					  const uint8_t *signature,
					  const uint8_t *challenge,
					  X509 **cert_out, EVP_PKEY **pkey_out)
{
	X509	   *cert = NULL;
	EVP_PKEY   *ephemeral_pkey = NULL;
	X509_NAME  *name = NULL;
	ASN1_INTEGER *serial = NULL;
	ASN1_OBJECT *fido2_oid = NULL;
	X509_EXTENSION *ext = NULL;
	ASN1_OCTET_STRING *ext_data = NULL;
	uint8_t	   *ext_der = NULL;
	int			ext_der_len;
	time_t		now;
	bool		success = false;

	*cert_out = NULL;
	*pkey_out = NULL;

	/* Create ephemeral key pair for TLS CertificateVerify */
	ephemeral_pkey = fido2_x509_create_ephemeral_key();
	if (!ephemeral_pkey)
		goto cleanup;

	/* Create certificate */
	cert = X509_new();
	if (!cert)
		goto cleanup;

	/* Set version to X.509v3 */
	if (!X509_set_version(cert, 2))
		goto cleanup;

	/* Set random serial number */
	serial = ASN1_INTEGER_new();
	if (!serial || !ASN1_INTEGER_set(serial, (long) time(NULL)))
		goto cleanup;
	if (!X509_set_serialNumber(cert, serial))
		goto cleanup;

	/* Set subject and issuer (self-signed) */
	name = X509_NAME_new();
	if (!name)
		goto cleanup;
	if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
									(unsigned char *) "FIDO2-Client", -1, -1, 0))
		goto cleanup;
	if (!X509_set_subject_name(cert, name) ||
		!X509_set_issuer_name(cert, name))
		goto cleanup;

	/* Set validity (now to now + 5 minutes) */
	now = time(NULL);
	if (!X509_time_adj_ex(X509_getm_notBefore(cert), 0, 0, &now) ||
		!X509_time_adj_ex(X509_getm_notAfter(cert), 0, 300, &now))
		goto cleanup;

	/* Set public key (ephemeral key, NOT the FIDO2 key) */
	if (!X509_set_pubkey(cert, ephemeral_pkey))
		goto cleanup;

	/* Create FIDO2 extension (includes the FIDO2 public key) */
	fido2_oid = OBJ_txt2obj(FIDO2_EXTENSION_OID, 1);
	if (!fido2_oid)
		goto cleanup;

	ext_der = fido2_x509_encode_assertion(pubkey_raw, flags, counter, signature,
										  challenge, &ext_der_len);
	if (!ext_der)
		goto cleanup;

	ext_data = ASN1_OCTET_STRING_new();
	if (!ext_data || !ASN1_OCTET_STRING_set(ext_data, ext_der, ext_der_len))
		goto cleanup;

	ext = X509_EXTENSION_create_by_OBJ(NULL, fido2_oid, 0, ext_data);
	if (!ext)
		goto cleanup;

	if (!X509_add_ext(cert, ext, -1))
		goto cleanup;

	/* Self-sign with ephemeral key */
	if (!X509_sign(cert, ephemeral_pkey, EVP_sha256()))
		goto cleanup;

	/* Return the certificate and ephemeral key to caller */
	*cert_out = cert;
	*pkey_out = ephemeral_pkey;
	cert = NULL;				/* Don't free on cleanup */
	ephemeral_pkey = NULL;		/* Don't free on cleanup */
	success = true;

cleanup:
	X509_free(cert);
	EVP_PKEY_free(ephemeral_pkey);
	X509_NAME_free(name);
	ASN1_INTEGER_free(serial);
	ASN1_OBJECT_free(fido2_oid);
	X509_EXTENSION_free(ext);
	ASN1_OCTET_STRING_free(ext_data);
	free(ext_der);

	return success;
}

/*
 * Parse FIDO2 assertion from an X.509 certificate extension.
 *
 * The FIDO2 public key is stored in the extension (not in the certificate's
 * Subject Public Key field, which contains an ephemeral key for TLS).
 *
 * Parameters:
 *   cert        - The X.509 certificate
 *   flags       - Output: FIDO2 authenticator flags
 *   counter     - Output: FIDO2 signature counter
 *   signature   - Output: 64-byte buffer for signature (r || s)
 *   challenge   - Output: 32-byte buffer for challenge
 *   pubkey      - Output: 65-byte buffer for FIDO2 public key
 *
 * Returns true on success, false if extension not found or parse error.
 */
bool
fido2_x509_parse_assertion(X509 *cert,
						   uint8_t *flags, uint32_t *counter,
						   uint8_t *signature, uint8_t *challenge,
						   uint8_t *pubkey)
{
	ASN1_OBJECT *fido2_oid = NULL;
	int			ext_idx;
	X509_EXTENSION *ext;
	ASN1_OCTET_STRING *ext_data;
	const uint8_t *p, *end;
	int			ext_len;

	/* Get the FIDO2 extension OID */
	fido2_oid = OBJ_txt2obj(FIDO2_EXTENSION_OID, 1);
	if (!fido2_oid)
		return false;

	/* Find the extension */
	ext_idx = X509_get_ext_by_OBJ(cert, fido2_oid, -1);
	ASN1_OBJECT_free(fido2_oid);

	if (ext_idx < 0)
		return false;

	ext = X509_get_ext(cert, ext_idx);
	if (!ext)
		return false;

	ext_data = X509_EXTENSION_get_data(ext);
	if (!ext_data)
		return false;

	p = ASN1_STRING_get0_data(ext_data);
	ext_len = ASN1_STRING_length(ext_data);
	end = p + ext_len;

	/* Parse SEQUENCE header */
	if (ext_len < 2 || *p != 0x30)
		return false;
	p++;

	/* Parse length */
	if (*p & 0x80)
	{
		int			len_bytes = *p & 0x7f;

		p++;
		if (len_bytes > 2 || p + len_bytes > end)
			return false;
		/* Skip length bytes - we'll parse until we hit expected sizes */
		p += len_bytes;
	}
	else
	{
		p++;
	}

	/* Parse pubkey (OCTET STRING, 65 bytes) */
	if (p + 2 > end || p[0] != 0x04 || p[1] != FIDO2_ES256_PUBKEY_LENGTH)
		return false;
	p += 2;
	if (p + FIDO2_ES256_PUBKEY_LENGTH > end)
		return false;
	memcpy(pubkey, p, FIDO2_ES256_PUBKEY_LENGTH);
	p += FIDO2_ES256_PUBKEY_LENGTH;

	/* Parse flags (OCTET STRING, 1 byte) */
	if (p + 3 > end || p[0] != 0x04 || p[1] != 0x01)
		return false;
	*flags = p[2];
	p += 3;

	/* Parse counter (INTEGER) */
	if (p + 2 > end || p[0] != 0x02)
		return false;
	{
		int			int_len = p[1];

		p += 2;
		if (p + int_len > end || int_len > 5)
			return false;

		*counter = 0;
		/* Handle leading zero for positive numbers */
		if (int_len > 0 && *p == 0x00)
		{
			p++;
			int_len--;
		}
		while (int_len > 0)
		{
			*counter = (*counter << 8) | *p++;
			int_len--;
		}
	}

	/* Parse signature (OCTET STRING, 64 bytes) */
	if (p + 2 > end || p[0] != 0x04 || p[1] != FIDO2_ES256_SIG_LENGTH)
		return false;
	p += 2;
	if (p + FIDO2_ES256_SIG_LENGTH > end)
		return false;
	memcpy(signature, p, FIDO2_ES256_SIG_LENGTH);
	p += FIDO2_ES256_SIG_LENGTH;

	/* Parse challenge (OCTET STRING, 32 bytes) */
	if (p + 2 > end || p[0] != 0x04 || p[1] != FIDO2_CHALLENGE_LENGTH)
		return false;
	p += 2;
	if (p + FIDO2_CHALLENGE_LENGTH > end)
		return false;
	memcpy(challenge, p, FIDO2_CHALLENGE_LENGTH);

	return true;
}

/*
 * Derive FIDO2 challenge from server's CertificateVerify signature.
 * challenge = SHA256(certificate_verify_signature)
 */
void
fido2_x509_derive_challenge(const uint8_t *server_cv, int server_cv_len,
							uint8_t *challenge)
{
	EVP_MD_CTX *ctx;

	ctx = EVP_MD_CTX_new();
	if (ctx)
	{
		unsigned int len = FIDO2_CHALLENGE_LENGTH;

		EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
		EVP_DigestUpdate(ctx, server_cv, server_cv_len);
		EVP_DigestFinal_ex(ctx, challenge, &len);
		EVP_MD_CTX_free(ctx);
	}
	else
	{
		/* Fallback: zero challenge (should never happen) */
		memset(challenge, 0, FIDO2_CHALLENGE_LENGTH);
	}
}

#endif							/* USE_OPENSSL */
