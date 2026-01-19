/*-------------------------------------------------------------------------
 *
 * auth-fido2.h
 *	  Interface to libpq/auth-fido2.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth-fido2.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTH_FIDO2_H
#define PG_AUTH_FIDO2_H

#include "libpq/sasl.h"

/* SASL implementation callbacks */
extern PGDLLIMPORT const pg_be_sasl_mech pg_be_fido2_mech;

#endif							/* PG_AUTH_FIDO2_H */
