/*-------------------------------------------------------------------------
 * fe-auth-passkey.h
 *	  Client-side Passkey SASL authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-passkey.h
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_PASSKEY_H
#define FE_AUTH_PASSKEY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Passkey assertion result from platform-specific code.
 * This structure is filled by the macOS/Windows-specific code
 * and used by the common SASL state machine.
 */
typedef struct PasskeyAssertion
{
	uint8_t	   *authenticator_data;
	size_t		authenticator_data_len;
	uint8_t	   *client_data_json;
	size_t		client_data_json_len;
	uint8_t	   *signature;
	size_t		signature_len;
	uint8_t	   *credential_id;
	size_t		credential_id_len;
	char	   *error_message;
} PasskeyAssertion;

/*
 * Platform-specific functions. These are implemented in:
 * - fe-auth-passkey-darwin.m (macOS)
 * - (future) fe-auth-passkey-win32.c (Windows)
 */

/*
 * Check if passkey authentication is supported on this platform.
 * Returns true if supported, false otherwise.
 */
extern bool pg_passkey_supported(void);

/*
 * Perform passkey assertion (signing).
 *
 * Parameters:
 *   rp_id: Relying Party ID
 *   challenge: The server challenge (binary)
 *   challenge_len: Length of challenge
 *   credential_id: Optional credential ID hint (for allowCredentials)
 *   credential_id_len: Length of credential_id (0 if not provided)
 *   allow_hybrid: Whether to show QR code for cross-device auth
 *
 * Returns:
 *   A PasskeyAssertion structure. On success, all fields except error_message
 *   are filled. On failure, error_message is set.
 *
 * The caller is responsible for freeing the assertion using pg_passkey_free_assertion().
 */
extern PasskeyAssertion *pg_passkey_get_assertion(const char *rp_id,
												  const uint8_t *challenge,
												  size_t challenge_len,
												  const uint8_t *credential_id,
												  size_t credential_id_len,
												  bool allow_hybrid);

/*
 * Free a PasskeyAssertion structure.
 */
extern void pg_passkey_free_assertion(PasskeyAssertion *assertion);

#endif							/* FE_AUTH_PASSKEY_H */
