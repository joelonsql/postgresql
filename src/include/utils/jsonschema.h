/*-------------------------------------------------------------------------
 *
 * jsonschema.h
 *	  Prototypes for JSON Schema generation functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/jsonschema.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JSONSCHEMA_H
#define JSONSCHEMA_H

#include "utils/jsonb.h"

/* Main worker function used internally */
extern Jsonb *json_schema_generate_worker(Oid funcid);

#endif							/* JSONSCHEMA_H */