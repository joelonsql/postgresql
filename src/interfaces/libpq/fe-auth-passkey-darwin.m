/*-------------------------------------------------------------------------
 * fe-auth-passkey-darwin.m
 *	  macOS-specific Passkey authentication using AuthenticationServices
 *
 * This implements passkey authentication using Apple's AuthenticationServices
 * framework, which provides native Touch ID and iCloud Keychain integration,
 * as well as QR code support for cross-device authentication.
 *
 * NOTE: When USE_OPENSSL is defined, we prefer caBLE (fe-auth-cable.c)
 * instead of AuthenticationServices because AuthenticationServices requires
 * app bundle entitlements that CLI tools like psql cannot have.
 *
 * This file is only compiled on macOS, and the functions are only used
 * when USE_OPENSSL is NOT defined.
 *
 * Requires macOS 13.3+ for ASAuthorizationPlatformPublicKeyCredentialProvider
 * assertion support.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-passkey-darwin.m
 *-------------------------------------------------------------------------
 */

/*
 * When USE_OPENSSL is defined, passkey authentication uses caBLE instead
 * of the native AuthenticationServices framework. caBLE works without
 * entitlements by displaying a QR code for cross-device authentication.
 */
#ifndef USE_OPENSSL

#import <Foundation/Foundation.h>
#import <AuthenticationServices/AuthenticationServices.h>
#import <AppKit/AppKit.h>

#include "fe-auth-passkey.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PGPASSKEYDEBUG_ENV "PGPASSKEYDEBUG"

#define passkey_debug(...) do { \
	if (getenv(PGPASSKEYDEBUG_ENV)) fprintf(stderr, __VA_ARGS__); \
} while(0)

/*
 * Check if we're running on macOS 13.3+ which supports the passkey APIs.
 */
static bool
check_macos_version(void)
{
	if (@available(macOS 13.3, *))
		return true;
	return false;
}

/*
 * Delegate class for handling ASAuthorizationController callbacks.
 * We track completion state and run the run loop until done.
 *
 * For CLI applications, we create a minimal window to serve as the
 * presentation anchor for the passkey dialog.
 */
API_AVAILABLE(macos(13.3))
@interface PGPasskeyDelegate : NSObject <ASAuthorizationControllerDelegate,
                                          ASAuthorizationControllerPresentationContextProviding>
@property (nonatomic) BOOL completed;
@property (nonatomic, strong) ASAuthorizationPlatformPublicKeyCredentialAssertion *result;
@property (nonatomic, strong) NSError *error;
@property (nonatomic, strong) NSWindow *window;
@end

API_AVAILABLE(macos(13.3))
@implementation PGPasskeyDelegate

- (instancetype)init
{
	self = [super init];
	if (self)
	{
		_completed = NO;
		_result = nil;
		_error = nil;

		/*
		 * Create a minimal window to serve as the presentation anchor.
		 * The passkey dialog will appear as a sheet attached to this window,
		 * or as a separate system dialog.
		 */
		NSRect frame = NSMakeRect(0, 0, 1, 1);
		_window = [[NSWindow alloc] initWithContentRect:frame
											  styleMask:NSWindowStyleMaskBorderless
												backing:NSBackingStoreBuffered
												  defer:NO];
		[_window setReleasedWhenClosed:NO];
		[_window center];

		/*
		 * We need to activate the application and make the window key
		 * for the passkey dialog to appear properly.
		 */
		[NSApp activateIgnoringOtherApps:YES];
		[_window makeKeyAndOrderFront:nil];
	}
	return self;
}

- (void)authorizationController:(ASAuthorizationController *)controller
   didCompleteWithAuthorization:(ASAuthorization *)authorization
{
	passkey_debug("PASSKEY-DARWIN: didCompleteWithAuthorization\n");
	if ([authorization.credential isKindOfClass:[ASAuthorizationPlatformPublicKeyCredentialAssertion class]])
	{
		self.result = (ASAuthorizationPlatformPublicKeyCredentialAssertion *)authorization.credential;
	}
	else
	{
		self.error = [NSError errorWithDomain:@"PostgreSQL"
										 code:-1
									 userInfo:@{NSLocalizedDescriptionKey: @"Unexpected credential type"}];
	}
	self.completed = YES;
}

- (void)authorizationController:(ASAuthorizationController *)controller
           didCompleteWithError:(NSError *)error
{
	passkey_debug("PASSKEY-DARWIN: didCompleteWithError: %s\n", [[error localizedDescription] UTF8String]);
	self.error = error;
	self.completed = YES;
}

- (ASPresentationAnchor)presentationAnchorForAuthorizationController:(ASAuthorizationController *)controller
{
	passkey_debug("PASSKEY-DARWIN: presentationAnchorForAuthorizationController returning window\n");
	return self.window;
}

@end

bool
pg_passkey_supported(void)
{
	return check_macos_version();
}

PasskeyAssertion *
pg_passkey_get_assertion(const char *rp_id,
						 const uint8_t *challenge,
						 size_t challenge_len,
						 const uint8_t *credential_id,
						 size_t credential_id_len,
						 bool allow_hybrid)
{
	PasskeyAssertion *assertion;

	assertion = calloc(1, sizeof(PasskeyAssertion));
	if (!assertion)
		return NULL;

	if (!check_macos_version())
	{
		assertion->error_message = strdup("passkey requires macOS 13.3 or later");
		return assertion;
	}

	passkey_debug("PASSKEY-DARWIN: starting assertion for rp_id=%s\n", rp_id);

	/*
	 * The AuthenticationServices API is async and requires a run loop.
	 * For CLI applications, we need to run the run loop ourselves while
	 * waiting for the callback.
	 */
	@autoreleasepool
	{
		/*
		 * Ensure NSApplication is initialized for CLI apps.
		 * This is required for the passkey UI to appear.
		 */
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		if (@available(macOS 13.3, *))
		{
			NSString *rpId = [NSString stringWithUTF8String:rp_id];
			NSData *challengeData = [NSData dataWithBytes:challenge length:challenge_len];

			/* Create credential provider */
			ASAuthorizationPlatformPublicKeyCredentialProvider *provider =
				[[ASAuthorizationPlatformPublicKeyCredentialProvider alloc]
					initWithRelyingPartyIdentifier:rpId];

			/* Create assertion request */
			ASAuthorizationPlatformPublicKeyCredentialAssertionRequest *request =
				[provider createCredentialAssertionRequestWithChallenge:challengeData];

			/* Set allowCredentials if we have a credential_id hint */
			if (credential_id && credential_id_len > 0)
			{
				NSData *credIdData = [NSData dataWithBytes:credential_id length:credential_id_len];
				ASAuthorizationPlatformPublicKeyCredentialDescriptor *descriptor =
					[[ASAuthorizationPlatformPublicKeyCredentialDescriptor alloc]
						initWithCredentialID:credIdData];
				request.allowedCredentials = @[descriptor];
				passkey_debug("PASSKEY-DARWIN: set allowedCredentials with %zu byte credential_id\n", credential_id_len);
			}

			/* Create authorization controller */
			PGPasskeyDelegate *delegate = [[PGPasskeyDelegate alloc] init];
			ASAuthorizationController *controller =
				[[ASAuthorizationController alloc] initWithAuthorizationRequests:@[request]];
			controller.delegate = delegate;
			controller.presentationContextProvider = delegate;

			passkey_debug("PASSKEY-DARWIN: calling performRequests\n");

			/*
			 * Perform the authorization request. This will show the native
			 * macOS passkey dialog with Touch ID, iCloud Keychain, and
			 * optionally QR code for cross-device auth.
			 */
			[controller performRequests];

			/*
			 * Run the run loop until we get a callback or timeout.
			 * This is necessary for CLI applications that don't have
			 * their own run loop running.
			 */
			NSDate *timeout = [NSDate dateWithTimeIntervalSinceNow:300.0];
			passkey_debug("PASSKEY-DARWIN: entering run loop\n");

			while (!delegate.completed && [[NSDate date] compare:timeout] == NSOrderedAscending)
			{
				[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
										 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
			}

			passkey_debug("PASSKEY-DARWIN: exited run loop, completed=%d\n", delegate.completed);

			if (!delegate.completed)
			{
				assertion->error_message = strdup("passkey authentication timed out");
				return assertion;
			}

			if (delegate.error)
			{
				const char *desc = [[delegate.error localizedDescription] UTF8String];
				if (desc)
					assertion->error_message = strdup(desc);
				else
					assertion->error_message = strdup("passkey authentication failed");
				return assertion;
			}

			if (!delegate.result)
			{
				assertion->error_message = strdup("no assertion result");
				return assertion;
			}

			/* Copy assertion data to our C structure */
			ASAuthorizationPlatformPublicKeyCredentialAssertion *cred = delegate.result;

			/* rawAuthenticatorData */
			NSData *authData = cred.rawAuthenticatorData;
			if (authData)
			{
				assertion->authenticator_data_len = [authData length];
				assertion->authenticator_data = malloc(assertion->authenticator_data_len);
				if (assertion->authenticator_data)
					memcpy(assertion->authenticator_data, [authData bytes], assertion->authenticator_data_len);
			}

			/* rawClientDataJSON */
			NSData *clientData = cred.rawClientDataJSON;
			if (clientData)
			{
				assertion->client_data_json_len = [clientData length];
				assertion->client_data_json = malloc(assertion->client_data_json_len);
				if (assertion->client_data_json)
					memcpy(assertion->client_data_json, [clientData bytes], assertion->client_data_json_len);
			}

			/* signature */
			NSData *sigData = cred.signature;
			if (sigData)
			{
				assertion->signature_len = [sigData length];
				assertion->signature = malloc(assertion->signature_len);
				if (assertion->signature)
					memcpy(assertion->signature, [sigData bytes], assertion->signature_len);
			}

			/* credentialID */
			NSData *credId = cred.credentialID;
			if (credId)
			{
				assertion->credential_id_len = [credId length];
				assertion->credential_id = malloc(assertion->credential_id_len);
				if (assertion->credential_id)
					memcpy(assertion->credential_id, [credId bytes], assertion->credential_id_len);
			}

			passkey_debug("PASSKEY-DARWIN: got assertion data: auth=%zu, client=%zu, sig=%zu, cred=%zu\n",
						  assertion->authenticator_data_len, assertion->client_data_json_len,
						  assertion->signature_len, assertion->credential_id_len);

			/* Verify we got all required data */
			if (!assertion->authenticator_data || !assertion->client_data_json ||
				!assertion->signature || !assertion->credential_id)
			{
				pg_passkey_free_assertion(assertion);
				assertion = calloc(1, sizeof(PasskeyAssertion));
				if (assertion)
					assertion->error_message = strdup("incomplete assertion data");
				return assertion;
			}
		}
	}

	return assertion;
}

void
pg_passkey_free_assertion(PasskeyAssertion *assertion)
{
	if (!assertion)
		return;
	free(assertion->authenticator_data);
	free(assertion->client_data_json);
	free(assertion->signature);
	free(assertion->credential_id);
	free(assertion->error_message);
	free(assertion);
}

#endif							/* !USE_OPENSSL */
