/*-------------------------------------------------------------------------
 *
 * notify_dispatcher.h
 *	  Notify dispatcher background worker definitions
 *
 * The notify dispatcher is a background worker that handles controlled
 * wakeup of LISTEN/NOTIFY listeners to prevent thundering herd.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/include/postmaster/notify_dispatcher.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NOTIFY_DISPATCHER_H
#define NOTIFY_DISPATCHER_H

extern void NotifyDispatcherRegister(void);
extern void NotifyDispatcherMain(Datum main_arg) pg_attribute_noreturn();

extern int notify_dispatcher_batch_size;
extern int notify_dispatcher_wake_interval;

#endif							/* NOTIFY_DISPATCHER_H */