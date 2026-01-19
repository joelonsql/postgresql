/* Copyright 2014, Kenneth MacKay. Licensed under the BSD 2-clause license. */

#ifndef _UECC_H_
#define _UECC_H_

#include <stdint.h>

/* Platform selection options.
If uECC_PLATFORM is not defined, the code will try to guess it based on compiler macros.
Possible values for uECC_PLATFORM are defined below: */
#define uECC_arch_other 0
#define uECC_x86        1
#define uECC_x86_64     2
#define uECC_arm        3
#define uECC_arm_thumb  4
#define uECC_arm_thumb2 5
#define uECC_arm64      6
#define uECC_avr        7

/* If desired, you can define uECC_WORD_SIZE as appropriate for your platform (1, 4, or 8 bytes).
If uECC_WORD_SIZE is not explicitly defined then it will be automatically set based on your
platform. */

/* Optimization level; trade speed for code size.
   Larger values produce code that is faster but larger.
   Currently supported values are 0 - 4; 0 is unusably slow for most applications.
   Optimization level 4 currently only has an effect ARM platforms where more than one
   curve is enabled. */
#ifndef uECC_OPTIMIZATION_LEVEL
    #define uECC_OPTIMIZATION_LEVEL 2
#endif

/* Curve support selection. Set to 0 to remove that curve. */
/* Only P-256 (secp256r1) is enabled for WebAuthn verification */
#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 1
#define uECC_SUPPORTS_secp256k1 0

struct uECC_Curve_t;
typedef const struct uECC_Curve_t * uECC_Curve;

#ifdef __cplusplus
extern "C"
{
#endif

uECC_Curve uECC_secp256r1(void);

/* uECC_curve_public_key_size() function.

Returns the size of a public key for the curve in bytes.
*/
int uECC_curve_public_key_size(uECC_Curve curve);

/* uECC_valid_public_key() function.
Check to see if a public key is valid.

Note that you are not required to check for a valid public key before using any other uECC
functions. However, you may wish to avoid spending CPU time computing a shared secret or
verifying a signature using an invalid public key.

Inputs:
    public_key - The public key to check.

Returns 1 if the public key is valid, 0 if it is invalid.
*/
int uECC_valid_public_key(const uint8_t *public_key, uECC_Curve curve);

/* uECC_verify() function.
Verify an ECDSA signature.

Usage: Compute the hash of the signed data using the same hash as the signer and
pass it to this function along with the signer's public key and the signature values (r and s).

Inputs:
    public_key   - The signer's public key.
    message_hash - The hash of the signed data.
    hash_size    - The size of message_hash in bytes.
    signature    - The signature value.

Returns 1 if the signature is valid, 0 if it is invalid.
*/
int uECC_verify(const uint8_t *public_key,
                const uint8_t *message_hash,
                unsigned hash_size,
                const uint8_t *signature,
                uECC_Curve curve);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* _UECC_H_ */
