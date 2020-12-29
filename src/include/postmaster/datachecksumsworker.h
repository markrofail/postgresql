/*-------------------------------------------------------------------------
 *
 * datachecksumsworker.h
 *	  header file for checksum helper background worker
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/datachecksumsworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATACHECKSUMSWORKER_H
#define DATACHECKSUMSWORKER_H

/* Shared memory */
extern Size DatachecksumsWorkerShmemSize(void);
extern void DatachecksumsWorkerShmemInit(void);

/* Status functions */
bool		DataChecksumsWorkerStarted(void);

/* Start the background processes for enabling checksums */
void		StartDatachecksumsWorkerLauncher(bool enable_checksums,
											 int cost_delay, int cost_limit);

/* Shutdown the background processes, if any */
void		ShutdownDatachecksumsWorkerIfRunning(void);

/* Background worker entrypoints */
void		DatachecksumsWorkerLauncherMain(Datum arg);
void		DatachecksumsWorkerMain(Datum arg);
void		ResetDataChecksumsStateInDatabase(Datum arg);

#endif							/* DATACHECKSUMSWORKER_H */
