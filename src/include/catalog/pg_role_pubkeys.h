/*-------------------------------------------------------------------------
 *
 * pg_role_pubkeys.h
 *	  definition of the "role public keys" system catalog (pg_role_pubkeys)
 *
 *	  This catalog stores sk-provider public keys for role authentication.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_role_pubkeys.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ROLE_PUBKEYS_H
#define PG_ROLE_PUBKEYS_H

#include "catalog/genbki.h"
#include "catalog/pg_role_pubkeys_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_role_pubkeys definition.  cpp turns this into
 *		typedef struct FormData_pg_role_pubkeys
 * ----------------
 */
CATALOG(pg_role_pubkeys,6500,RolePubkeysRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(6501,RolePubkeysRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */
	Oid			roleid BKI_LOOKUP(pg_authid);	/* ID of the role */
	NameData	key_name;		/* user-friendly credential label */
	int16		algorithm;		/* COSE algorithm identifier (-7 = ES256) */
	int16		credential_type;	/* 1=FIDO2/sk-api, 2=WebAuthn/Passkey */

	/*
	 * Variable-length fields; must use heap_getattr() to access.
	 */
#ifdef CATALOG_VARLEN
	bytea		public_key BKI_FORCE_NOT_NULL;		/* raw public key bytes (65 bytes for ES256 uncompressed) */
	text		keystring BKI_FORCE_NOT_NULL;		/* original OpenSSH keystring */
	bytea		credential_id;		/* WebAuthn credential ID (for allowCredentials) */
	text		rp_id;				/* relying party ID used at registration */
	timestamptz	enrolled_at BKI_DEFAULT(now);		/* when the key was registered */
#endif
} FormData_pg_role_pubkeys;

/* ----------------
 *		Form_pg_role_pubkeys corresponds to a pointer to a tuple with
 *		the format of pg_role_pubkeys relation.
 * ----------------
 */
typedef FormData_pg_role_pubkeys *Form_pg_role_pubkeys;

DECLARE_TOAST_WITH_MACRO(pg_role_pubkeys, 6505, 6506, PgRolePubkeysToastTable, PgRolePubkeysToastIndex);

DECLARE_UNIQUE_INDEX_PKEY(pg_role_pubkeys_oid_index, 6502, RolePubkeysOidIndexId, pg_role_pubkeys, btree(oid oid_ops));
DECLARE_INDEX(pg_role_pubkeys_roleid_index, 6503, RolePubkeysRoleidIndexId, pg_role_pubkeys, btree(roleid oid_ops));
DECLARE_UNIQUE_INDEX(pg_role_pubkeys_roleid_keyname_index, 6504, RolePubkeysRoleidKeynameIndexId, pg_role_pubkeys, btree(roleid oid_ops, key_name name_ops));

MAKE_SYSCACHE(ROLEPUBKEYSOID, pg_role_pubkeys_oid_index, 4);
MAKE_SYSCACHE(ROLEPUBKEYSROLEID, pg_role_pubkeys_roleid_index, 8);

/* COSE algorithm identifiers */
#define COSE_ALG_ES256		(-7)	/* ECDSA w/ SHA-256 on P-256 curve */

/* Credential type identifiers */
#define CRED_TYPE_FIDO2		1	/* FIDO2/sk-api (OpenSSH compatible) */
#define CRED_TYPE_WEBAUTHN	2	/* WebAuthn/Passkey (native API) */

#endif							/* PG_ROLE_PUBKEYS_H */
