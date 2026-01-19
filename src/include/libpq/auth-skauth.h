/*-------------------------------------------------------------------------
 *
 * auth-skauth.h
 *	  Interface to libpq/auth-skauth.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth-skauth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTH_SKAUTH_H
#define PG_AUTH_SKAUTH_H

#ifdef USE_OPENSSL

#include "libpq/sasl.h"

/* SASL implementation callbacks */
extern PGDLLIMPORT const pg_be_sasl_mech pg_be_skauth_mech;

#endif							/* USE_OPENSSL */

#endif							/* PG_AUTH_SKAUTH_H */
