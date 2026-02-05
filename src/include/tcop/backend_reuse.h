/*-------------------------------------------------------------------------
 *
 * backend_reuse.h
 *	  Declarations for backend connection reuse (pooling).
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/tcop/backend_reuse.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKEND_REUSE_H
#define BACKEND_REUSE_H

extern bool BackendEnterPooledState(void);

#endif							/* BACKEND_REUSE_H */
