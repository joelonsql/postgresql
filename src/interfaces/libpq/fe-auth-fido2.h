/*-------------------------------------------------------------------------
 * fe-auth-fido2.h
 *	  Client-side FIDO2 SASL authentication - OpenSSH sk-api definitions
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-fido2.h
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_FIDO2_H
#define FE_AUTH_FIDO2_H

#include <stddef.h>
#include <stdint.h>

/* OpenSSH sk-api version and algorithm constants */
#define SSH_SK_VERSION_MAJOR		0x000a0000
#define SSH_SK_VERSION_MAJOR_MASK	0xffff0000
#define SSH_SK_ECDSA				0x00
#define SSH_SK_USER_PRESENCE_REQD	0x01
#define SSH_SK_USER_VERIFICATION_REQD	0x04

/* OpenSSH sk-api structures */
struct sk_sign_response {
	uint8_t		flags;
	uint32_t	counter;
	uint8_t	   *sig_r;
	size_t		sig_r_len;
	uint8_t	   *sig_s;
	size_t		sig_s_len;
};

struct sk_option {
	char	   *name;
	char	   *value;
	uint8_t		required;
};

struct sk_enroll_response {
	uint8_t		flags;
	uint8_t	   *public_key;
	size_t		public_key_len;
	uint8_t	   *key_handle;
	size_t		key_handle_len;
	uint8_t	   *signature;
	size_t		signature_len;
	uint8_t	   *attestation_cert;
	size_t		attestation_cert_len;
	uint8_t	   *authdata;
	size_t		authdata_len;
};

struct sk_resident_key {
	uint32_t	alg;
	size_t		slot;
	char	   *application;
	struct sk_enroll_response key;
	uint8_t		flags;
	uint8_t	   *user_id;
	size_t		user_id_len;
};

/* OpenSSH sk-api function pointer typedefs */
typedef uint32_t (*sk_api_version_fn)(void);
typedef int (*sk_sign_fn)(uint32_t alg, const uint8_t *data, size_t data_len,
						  const char *application,
						  const uint8_t *key_handle, size_t key_handle_len,
						  uint8_t flags, const char *pin,
						  struct sk_option **options,
						  struct sk_sign_response **sign_response);
typedef int (*sk_load_resident_keys_fn)(const char *pin,
										struct sk_option **options,
										struct sk_resident_key ***rks,
										size_t *nrks);
typedef void (*sk_free_sign_response_fn)(struct sk_sign_response *response);
typedef void (*sk_free_resident_keys_fn)(struct sk_resident_key **rks,
										 size_t nrks);

#endif							/* FE_AUTH_FIDO2_H */
