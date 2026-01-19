/*-------------------------------------------------------------------------
 *
 * sk-provider.h
 *	  Security Key provider interface for FIDO2/WebAuthn authentication
 *
 * This header defines the pluggable interface that security key provider
 * libraries must implement. Providers are loaded via dlopen at runtime,
 * similar to OpenSSH's sk-provider mechanism.
 *
 * Compatible providers include:
 *   - libfido2 wrapper for USB FIDO2 tokens (YubiKey, etc.)
 *   - ssh-keychain.dylib for macOS Secure Enclave
 *   - Custom implementations
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/sk-provider.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SK_PROVIDER_H
#define SK_PROVIDER_H

#include <stdint.h>
#include <stddef.h>

/*
 * API version - providers must return this from pg_sk_api_version()
 */
#define PG_SK_API_VERSION		1

/*
 * Algorithm identifiers (COSE algorithm numbers)
 */
#define PG_SK_ALG_ES256			(-7)	/* ECDSA P-256 with SHA-256 */

/*
 * Error codes returned by provider functions
 */
#define PG_SK_ERR_SUCCESS			0
#define PG_SK_ERR_GENERAL			-1	/* General/unknown error */
#define PG_SK_ERR_NO_DEVICE			-2	/* No device found */
#define PG_SK_ERR_TIMEOUT			-3	/* Operation timed out */
#define PG_SK_ERR_NO_CREDENTIALS	-4	/* No matching credential */
#define PG_SK_ERR_PIN_REQUIRED		-5	/* PIN required but not provided */
#define PG_SK_ERR_PIN_INVALID		-6	/* Invalid PIN */
#define PG_SK_ERR_UNSUPPORTED		-7	/* Unsupported operation */
#define PG_SK_ERR_NO_MEMORY			-8	/* Memory allocation failed */
#define PG_SK_ERR_CANCELLED			-9	/* Operation cancelled by user */

/*
 * Flags for sign operations
 */
#define PG_SK_FLAG_REQUIRE_UP		0x01	/* Require user presence */
#define PG_SK_FLAG_REQUIRE_UV		0x02	/* Require user verification */

/*
 * Parameters for key enrollment (generating a new credential)
 *
 * Note: Key enrollment is typically done outside PostgreSQL using
 * ssh-keygen or similar tools. This structure is provided for
 * completeness but may not be used by all providers.
 */
typedef struct pg_sk_enroll_params
{
	const char *application;		/* Relying party ID (e.g., "ssh:" or "pg:") */
	const uint8_t *challenge;		/* Random challenge (32 bytes) */
	size_t		challenge_len;
	const char *device;				/* Device path hint (may be NULL) */
	const char *pin;				/* Device PIN (may be NULL) */
	int			algorithm;			/* COSE algorithm identifier */
	uint32_t	flags;				/* PG_SK_FLAG_* */
} pg_sk_enroll_params;

/*
 * Public key output from enrollment
 */
typedef struct pg_sk_pubkey
{
	int			algorithm;			/* COSE algorithm identifier */
	uint8_t	   *public_key;			/* Public key in COSE Key format */
	size_t		public_key_len;
	uint8_t	   *key_handle;			/* Authenticator credential ID */
	size_t		key_handle_len;
	uint8_t	   *attestation_cert;	/* Attestation certificate (may be NULL) */
	size_t		attestation_cert_len;
	uint8_t	   *signature;			/* Attestation signature (may be NULL) */
	size_t		signature_len;
} pg_sk_pubkey;

/*
 * Parameters for signing operation
 */
typedef struct pg_sk_sign_params
{
	const char *application;		/* Relying party ID */
	const uint8_t *challenge;		/* Challenge to sign (32 bytes) */
	size_t		challenge_len;
	const uint8_t *key_handle;		/* Credential ID */
	size_t		key_handle_len;
	const char *device;				/* Device path hint (may be NULL) */
	const char *pin;				/* Device PIN (may be NULL) */
	uint32_t	flags;				/* PG_SK_FLAG_* */
} pg_sk_sign_params;

/*
 * Signature output from signing operation
 */
typedef struct pg_sk_signature
{
	uint8_t		flags;				/* Authenticator data flags */
	uint32_t	counter;			/* Signature counter */
	uint8_t	   *signature;			/* Signature (raw format: R || S for ES256) */
	size_t		signature_len;
} pg_sk_signature;

/*
 * Provider API function types
 *
 * A provider library must export these functions:
 *
 *   int pg_sk_api_version(void);
 *     Returns PG_SK_API_VERSION if compatible.
 *
 *   int pg_sk_enroll(const pg_sk_enroll_params *params, pg_sk_pubkey *out);
 *     Generate a new credential on the authenticator.
 *     Returns PG_SK_ERR_SUCCESS on success.
 *
 *   int pg_sk_sign(const pg_sk_sign_params *params, pg_sk_signature *out);
 *     Sign a challenge using an existing credential.
 *     Returns PG_SK_ERR_SUCCESS on success.
 *
 *   void pg_sk_free_pubkey(pg_sk_pubkey *pk);
 *     Free resources in pg_sk_pubkey (not the struct itself).
 *
 *   void pg_sk_free_signature(pg_sk_signature *sig);
 *     Free resources in pg_sk_signature (not the struct itself).
 *
 *   const char *pg_sk_strerror(int error);
 *     Return error message for an error code.
 */

typedef int (*pg_sk_api_version_fn)(void);
typedef int (*pg_sk_enroll_fn)(const pg_sk_enroll_params *params,
							   pg_sk_pubkey *out);
typedef int (*pg_sk_sign_fn)(const pg_sk_sign_params *params,
							 pg_sk_signature *out);
typedef void (*pg_sk_free_pubkey_fn)(pg_sk_pubkey *pk);
typedef void (*pg_sk_free_signature_fn)(pg_sk_signature *sig);
typedef const char *(*pg_sk_strerror_fn)(int error);

/*
 * Loaded provider handle
 */
typedef struct pg_sk_provider
{
	void	   *handle;				/* dlopen handle */
	pg_sk_api_version_fn api_version;
	pg_sk_enroll_fn enroll;
	pg_sk_sign_fn sign;
	pg_sk_free_pubkey_fn free_pubkey;
	pg_sk_free_signature_fn free_signature;
	pg_sk_strerror_fn strerror;
} pg_sk_provider;

/*
 * Load a security key provider library
 *
 * path: Path to the shared library (e.g., "/usr/lib/libfido2.so")
 * provider: Output provider handle
 * errmsg: Output error message on failure
 *
 * Returns true on success, false on failure.
 */
extern bool pg_sk_load_provider(const char *path, pg_sk_provider *provider,
								char **errmsg);

/*
 * Unload a security key provider
 */
extern void pg_sk_unload_provider(pg_sk_provider *provider);

#endif							/* SK_PROVIDER_H */
