/*-------------------------------------------------------------------------
 * fe-auth-cable-ble-darwin.m
 *	  caBLE BLE scanner for macOS using CoreBluetooth framework
 *
 * This module scans for BLE advertisements from the phone during caBLE
 * passkey authentication. The phone broadcasts an encrypted EID (Encrypted
 * IDentifier) containing the routing_id needed to connect to the tunnel.
 *
 * BLE Advertisement format (FIDO caBLE):
 *   Service UUID: 0xFFF9 (FIDO Alliance caBLE service)
 *   Service Data: 20 bytes encrypted EID
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-cable-ble-darwin.m
 *-------------------------------------------------------------------------
 */

/*
 * Include macOS headers first to avoid type conflicts with PostgreSQL.
 * macOS MacTypes.h defines 'Size' as 'long', while PostgreSQL c.h defines
 * it as 'size_t'. We cannot include both, so we avoid postgres_fe.h entirely
 * and declare only what we need.
 */
#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

/* Forward declaration - implemented in fe-auth-cable-eid.c */
extern int cable_eid_decrypt(const uint8_t *advert, const uint8_t *eid_key,
							 uint8_t *routing_id, uint16_t *tunnel_domain,
							 uint8_t *advert_plaintext);

/* FIDO caBLE BLE service UUID (short form: 0xFFF9) */
static NSString * const kCableBLEServiceUUID = @"FFF9";

/* EID advertisement length */
#define CABLE_EID_ADVERT_LENGTH 20

/* Routing ID length (from cable.h) */
#define CABLE_ROUTING_ID_LENGTH 3

/*
 * BLE Scanner state - shared between C code and Objective-C delegate
 */
typedef struct CableBLEScanner
{
	uint8_t		eid_key[64];			/* Key for decrypting EID */
	uint8_t		found_routing_id[3];	/* Extracted routing_id */
	uint16_t	found_domain;			/* Extracted tunnel domain */
	uint8_t		found_advert_plaintext[16];	/* Full decrypted EID for PSK */
	volatile bool found;				/* Set when valid EID found */
	volatile bool scanning;				/* Scanner is active */
	volatile bool powered_on;			/* Bluetooth is powered on */
	volatile bool error;				/* Error occurred */
	char		error_message[256];		/* Error description */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} CableBLEScanner;

static CableBLEScanner *g_scanner = NULL;

/*
 * Objective-C BLE delegate class
 */
@interface CableBLEDelegate : NSObject <CBCentralManagerDelegate>
{
	CableBLEScanner *_scanner;
}
@property (strong, nonatomic) CBCentralManager *centralManager;
@property (strong, nonatomic) CBUUID *cableServiceUUID;
@property (strong, nonatomic) dispatch_queue_t bleQueue;

- (instancetype)initWithScanner:(CableBLEScanner *)scanner;
- (void)stopScanning;

@end

@implementation CableBLEDelegate

- (instancetype)initWithScanner:(CableBLEScanner *)scanner
{
	self = [super init];
	if (self)
	{
		_scanner = scanner;

		/* Create the service UUID - use explicit retain to ensure it survives */
		_cableServiceUUID = [[CBUUID UUIDWithString:kCableBLEServiceUUID] retain];

		/*
		 * Create a dedicated dispatch queue for BLE operations.
		 * This allows BLE callbacks to proceed without blocking the main thread.
		 */
		_bleQueue = dispatch_queue_create("postgresql.cable.ble",
										  DISPATCH_QUEUE_SERIAL);

		/*
		 * Create the central manager on our dedicated queue.
		 * The delegate callbacks will be invoked on this queue.
		 */
		NSDictionary *options = @{CBCentralManagerOptionShowPowerAlertKey: @NO};
		_centralManager = [[CBCentralManager alloc] initWithDelegate:self
															   queue:_bleQueue
															 options:options];
	}
	return self;
}

- (void)dealloc
{
	if (_centralManager)
	{
		[_centralManager stopScan];
		[_centralManager release];
	}
	[_cableServiceUUID release];
	[_bleQueue release];
	[super dealloc];
}

/*
 * Called when Bluetooth state changes.
 * This is invoked on _bleQueue.
 */
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	if (!_scanner)
		return;

	pthread_mutex_lock(&_scanner->mutex);

	switch (central.state)
	{
		case CBManagerStatePoweredOn:
		{
			_scanner->powered_on = true;
			fprintf(stderr, "[BLE] Bluetooth powered on, starting scan for caBLE advertisements...\n");

			/*
			 * Start scanning for FIDO caBLE service (UUID 0xFFF9).
			 * We enable duplicate reporting to catch repeated advertisements.
			 * Use the retained _cableServiceUUID property.
			 */
			CBUUID *serviceUUID = self.cableServiceUUID;
			if (serviceUUID)
			{
				NSArray *services = @[serviceUUID];
				NSDictionary *options = @{
					CBCentralManagerScanOptionAllowDuplicatesKey: @YES
				};
				[central scanForPeripheralsWithServices:services options:options];
			}
			break;
		}

		case CBManagerStatePoweredOff:
			fprintf(stderr, "[BLE] Bluetooth is powered off\n");
			_scanner->error = true;
			snprintf(_scanner->error_message, sizeof(_scanner->error_message),
					 "Bluetooth is powered off");
			break;

		case CBManagerStateUnauthorized:
			fprintf(stderr, "[BLE] Bluetooth access not authorized\n");
			_scanner->error = true;
			snprintf(_scanner->error_message, sizeof(_scanner->error_message),
					 "Bluetooth access not authorized - check System Settings > Privacy & Security > Bluetooth");
			break;

		case CBManagerStateUnsupported:
			fprintf(stderr, "[BLE] Bluetooth LE not supported on this device\n");
			_scanner->error = true;
			snprintf(_scanner->error_message, sizeof(_scanner->error_message),
					 "Bluetooth LE not supported on this device");
			break;

		case CBManagerStateResetting:
			fprintf(stderr, "[BLE] Bluetooth is resetting...\n");
			break;

		case CBManagerStateUnknown:
		default:
			fprintf(stderr, "[BLE] Bluetooth state unknown\n");
			break;
	}

	pthread_cond_signal(&_scanner->cond);
	pthread_mutex_unlock(&_scanner->mutex);
}

/*
 * Called when a BLE peripheral (advertisement) is discovered.
 * We look for FIDO caBLE service data and try to decrypt it.
 * This is invoked on _bleQueue.
 */
- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
	 advertisementData:(NSDictionary<NSString *, id> *)advertisementData
				  RSSI:(NSNumber *)RSSI
{
	if (!_scanner)
		return;

	/*
	 * Look for service data associated with the FIDO caBLE service UUID.
	 * The 20-byte encrypted EID is in the service data, not manufacturer data.
	 */
	NSDictionary *serviceData = advertisementData[CBAdvertisementDataServiceDataKey];
	if (!serviceData)
		return;

	CBUUID *serviceUUID = self.cableServiceUUID;
	if (!serviceUUID)
		return;

	NSData *eidData = serviceData[serviceUUID];
	if (!eidData || eidData.length != CABLE_EID_ADVERT_LENGTH)
		return;

	/*
	 * Found a caBLE advertisement - try to decrypt it.
	 * If the HMAC matches, this is the phone that scanned our QR code.
	 */
	uint8_t routing_id[CABLE_ROUTING_ID_LENGTH];
	uint16_t tunnel_domain;
	uint8_t advert_plaintext[16];

	if (cable_eid_decrypt(eidData.bytes, _scanner->eid_key,
						  routing_id, &tunnel_domain, advert_plaintext) == 0)
	{
		pthread_mutex_lock(&_scanner->mutex);

		if (!_scanner->found)
		{
			memcpy(_scanner->found_routing_id, routing_id, CABLE_ROUTING_ID_LENGTH);
			_scanner->found_domain = tunnel_domain;
			memcpy(_scanner->found_advert_plaintext, advert_plaintext, 16);
			_scanner->found = true;

			fprintf(stderr, "[BLE] Found matching caBLE advertisement!\n");
			fprintf(stderr, "[BLE] Routing ID: %02x%02x%02x\n",
					routing_id[0], routing_id[1], routing_id[2]);
			fprintf(stderr, "[BLE] Tunnel domain: %s\n",
					tunnel_domain == 0 ? "cable.ua5v.com (Google)" :
					tunnel_domain == 1 ? "cable.auth.com (Apple)" : "unknown");
			fprintf(stderr, "[BLE] RSSI: %d dBm\n", RSSI.intValue);

			/* Stop scanning */
			[central stopScan];
		}

		pthread_cond_signal(&_scanner->cond);
		pthread_mutex_unlock(&_scanner->mutex);
	}
}

- (void)stopScanning
{
	if (_centralManager && _centralManager.state == CBManagerStatePoweredOn)
	{
		[_centralManager stopScan];
	}
}

@end

/* Global delegate - must be retained to keep BLE scanning active */
static CableBLEDelegate *g_delegate = NULL;

/*
 * Securely zero memory - use memset_s or volatile pointer trick
 */
static void
secure_bzero(void *ptr, size_t len)
{
	volatile unsigned char *p = (volatile unsigned char *)ptr;
	while (len--)
		*p++ = 0;
}

/*
 * Start BLE scanning for caBLE advertisements.
 *
 * The eid_key is derived from the QR secret and used to decrypt EID
 * advertisements. Only the phone that scanned our QR code will produce
 * a valid EID (HMAC will match).
 *
 * Returns 0 on success, -1 on error.
 */
int
cable_ble_start_scan(const uint8_t *eid_key, size_t key_len)
{
	if (key_len != 64)
	{
		fprintf(stderr, "[BLE] Invalid EID key length: %zu (expected 64)\n", key_len);
		return -1;
	}

	/* Allocate scanner state */
	g_scanner = calloc(1, sizeof(CableBLEScanner));
	if (!g_scanner)
		return -1;

	memcpy(g_scanner->eid_key, eid_key, 64);
	g_scanner->found = false;
	g_scanner->scanning = true;
	g_scanner->powered_on = false;
	g_scanner->error = false;

	pthread_mutex_init(&g_scanner->mutex, NULL);
	pthread_cond_init(&g_scanner->cond, NULL);

	/*
	 * Create the Objective-C delegate inside an autorelease pool.
	 * The delegate will be retained by g_delegate.
	 */
	@autoreleasepool {
		g_delegate = [[CableBLEDelegate alloc] initWithScanner:g_scanner];
		if (!g_delegate)
		{
			pthread_mutex_destroy(&g_scanner->mutex);
			pthread_cond_destroy(&g_scanner->cond);
			free(g_scanner);
			g_scanner = NULL;
			return -1;
		}
	}

	fprintf(stderr, "[BLE] BLE scanner initialized, waiting for Bluetooth to power on...\n");
	return 0;
}

/*
 * Wait for a caBLE BLE advertisement.
 *
 * Blocks until either:
 *   - A valid EID is found (returns 0, routing_id and advert_plaintext filled in)
 *   - Timeout expires (returns -1)
 *   - Error occurs (returns -1)
 *
 * The advert_plaintext (16 bytes) is the full decrypted EID, needed for
 * PSK derivation per FIDO CTAP 2.3. If NULL, it won't be copied.
 *
 * The timeout is in seconds.
 */
int
cable_ble_wait_for_advert(uint8_t *routing_id, uint16_t *tunnel_domain,
						  uint8_t *advert_plaintext, int timeout_secs)
{
	struct timespec abstime;
	int ret = -1;

	if (!g_scanner)
	{
		fprintf(stderr, "[BLE] Scanner not initialized\n");
		return -1;
	}

	/* Calculate absolute timeout */
	clock_gettime(CLOCK_REALTIME, &abstime);
	abstime.tv_sec += timeout_secs;

	pthread_mutex_lock(&g_scanner->mutex);

	/* Wait for Bluetooth to power on or error */
	while (!g_scanner->powered_on && !g_scanner->error && g_scanner->scanning)
	{
		int wait_ret = pthread_cond_timedwait(&g_scanner->cond, &g_scanner->mutex, &abstime);
		if (wait_ret == ETIMEDOUT)
		{
			fprintf(stderr, "[BLE] Timeout waiting for Bluetooth to power on\n");
			pthread_mutex_unlock(&g_scanner->mutex);
			return -1;
		}
	}

	if (g_scanner->error)
	{
		fprintf(stderr, "[BLE] Error: %s\n", g_scanner->error_message);
		pthread_mutex_unlock(&g_scanner->mutex);
		return -1;
	}

	/* Wait for advertisement or timeout */
	while (!g_scanner->found && !g_scanner->error && g_scanner->scanning)
	{
		int wait_ret = pthread_cond_timedwait(&g_scanner->cond, &g_scanner->mutex, &abstime);
		if (wait_ret == ETIMEDOUT)
		{
			fprintf(stderr, "[BLE] Timeout waiting for caBLE advertisement (phone may not have received QR data)\n");
			pthread_mutex_unlock(&g_scanner->mutex);
			return -1;
		}
	}

	if (g_scanner->found)
	{
		memcpy(routing_id, g_scanner->found_routing_id, CABLE_ROUTING_ID_LENGTH);
		*tunnel_domain = g_scanner->found_domain;
		if (advert_plaintext != NULL)
			memcpy(advert_plaintext, g_scanner->found_advert_plaintext, 16);
		ret = 0;
	}
	else if (g_scanner->error)
	{
		fprintf(stderr, "[BLE] Error during scan: %s\n", g_scanner->error_message);
	}

	pthread_mutex_unlock(&g_scanner->mutex);
	return ret;
}

/*
 * Stop BLE scanning and clean up resources.
 */
void
cable_ble_stop_scan(void)
{
	if (!g_scanner)
		return;

	pthread_mutex_lock(&g_scanner->mutex);
	g_scanner->scanning = false;
	pthread_cond_signal(&g_scanner->cond);
	pthread_mutex_unlock(&g_scanner->mutex);

	@autoreleasepool {
		if (g_delegate)
		{
			[g_delegate stopScanning];
			g_delegate = nil;
		}
	}

	pthread_mutex_destroy(&g_scanner->mutex);
	pthread_cond_destroy(&g_scanner->cond);

	/* Clear sensitive key material */
	secure_bzero(g_scanner->eid_key, sizeof(g_scanner->eid_key));
	free(g_scanner);
	g_scanner = NULL;

	fprintf(stderr, "[BLE] Scanner stopped and cleaned up\n");
}
