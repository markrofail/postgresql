/*
 * fdwxact.h
 *
 * PostgreSQL distributed transaction manager
 *
 * Portions Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * src/include/access/fdwxact.h
 */
#ifndef FDW_XACT_H
#define FDW_XACT_H

#include "access/fdwxact_xlog.h"
#include "access/xlogreader.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/execnodes.h"
#include "storage/backendid.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

#define	FDW_XACT_NOT_WAITING		0
#define	FDW_XACT_WAITING			1
#define	FDW_XACT_WAITING_RETRY		2
#define	FDW_XACT_WAIT_COMPLETE		3

#define FdwXactEnabled() (max_prepared_foreign_xacts > 0)

/* Maximum length of the prepared transaction id, borrowed from twophase.c */
#define FDW_XACT_ID_MAX_LEN 200

/* Enum to track the status of prepared foreign transaction */
typedef enum
{
	FDW_XACT_INITIAL,
	FDW_XACT_PREPARING,					/* foreign transaction is being prepared */
	FDW_XACT_PREPARED,					/* foreign transaction is prepared */
	FDW_XACT_COMMITTING_PREPARED,		/* foreign prepared transaction is to
										 * be committed */
	FDW_XACT_ABORTING_PREPARED, /* foreign prepared transaction is to be
								 * aborted */
} FdwXactStatus;


/* Enum for distributed_atomic_commit parameter */
typedef enum
{
	DISTRIBUTED_ATOMIC_COMMIT_DISABLED,	/* disable distributed atomic commit */
	DISTRIBUTED_ATOMIC_COMMIT_PREFER,	/* use twophase commit where available */
	DISTRIBUTED_ATOMIC_COMMIT_REQUIRED	/* all foreign servers have to support twophase
										 * commit */
} DistributedAtomicCommitLevel;

/* Shared memory entry for a prepared or being prepared foreign transaction */
typedef struct FdwXactData *FdwXact;

typedef struct FdwXactData
{
	FdwXact		fxact_free_next;	/* Next free FdwXact entry */
	FdwXact		fxact_next;			/* Pointer to the neext FdwXact entry accosiated
									 * with the same transaction */
	Oid				dbid;			/* database oid where to find foreign server
									 * and user mapping */
	TransactionId	local_xid;		/* XID of local transaction */
	Oid				serverid;		/* foreign server where transaction takes place */
	Oid				userid;			/* user who initiated the foreign transaction */
	Oid				umid;
	FdwXactStatus 	status;			/* The state of the foreign transaction. This
									 * doubles as the action to be taken on this entry. */

	/*
	 * Note that we need to keep track of two LSNs for each FdwXact. We keep
	 * track of the start LSN because this is the address we must use to read
	 * state data back from WAL when committing a FdwXact. We keep track of
	 * the end LSN because that is the LSN we need to wait for prior to
	 * commit.
	 */
	XLogRecPtr	insert_start_lsn;		/* XLOG offset of inserting this entry start */
	XLogRecPtr	insert_end_lsn;		/* XLOG offset of inserting this entry end */

	bool		valid;			/* has the entry been complete and written to file? */
	BackendId	held_by;		/* backend who are holding */
	bool		ondisk;			/* true if prepare state file is on disk */
	bool		inredo;			/* true if entry was added via xlog_redo */
	char		fdw_xact_id[FDW_XACT_MAX_ID_LEN];		/* prepared transaction identifier */
} FdwXactData;

/* Shared memory layout for maintaining foreign prepared transaction entries. */
typedef struct
{
	/* Head of linked list of free FdwXactData structs */
	FdwXact		freeFdwXacts;

	/* Number of valid foreign transaction entries */
	int			numFdwXacts;

	/* Upto max_prepared_foreign_xacts entries in the array */
	FdwXact		fdw_xacts[FLEXIBLE_ARRAY_MEMBER];		/* Variable length array */
} FdwXactCtlData;

/* Pointer to the shared memory holding the foreign transactions data */
FdwXactCtlData *FdwXactCtl;

typedef struct FdwXactState
{
	Oid		serverid;
	Oid		userid;
	Oid		umid;
	char	*fdwxact_id;
	void	*fdw_state;		/* foreign-data wrapper can keep state here */
} FdwXactState;

/* GUC parameters */
extern int	max_prepared_foreign_xacts;
extern int	max_foreign_xact_resolvers;
extern int	foreign_xact_resolution_retry_interval;
extern int	foreign_xact_resolver_timeout;
extern int	distributed_atomic_commit;

extern Size FdwXactShmemSize(void);
extern void FdwXactShmemInit(void);
extern void restoreFdwXactData(void);
extern TransactionId PrescanFdwXacts(TransactionId oldestActiveXid);
extern void RecoverFdwXacts(void);
extern void AtEOXact_FdwXacts(bool is_commit);
extern void AtPrepare_FdwXacts(void);
extern bool fdw_xact_exists(TransactionId xid, Oid dboid, Oid serverid,
				Oid userid);
extern void CheckPointFdwXacts(XLogRecPtr redo_horizon);
extern bool FdwTwoPhaseNeeded(void);
extern void PreCommit_FdwXacts(void);
extern void KnownFdwXactRecreateFiles(XLogRecPtr redo_horizon);
extern void FdwXactWaitToBeResolved(TransactionId wait_xid, bool commit);
extern bool FdwXactResolveDistributedTransaction(Oid dbid, bool is_active);
extern void FdwXactResolveAllDanglingTransactions(Oid dbid);
extern bool FdwXactIsAtomicCommitReady(void);
extern void FdwXactCleanupAtProcExit(void);
extern void RegisterFdwXactByRelId(Oid relid, bool modified);
extern void RegisterFdwXactByServerId(Oid serverid, bool modified);
extern void FdwXactMarkForeignServerAccessed(Oid relid, bool modified);
extern bool check_distributed_atomic_commit(int *newval, void **extra,
										  GucSource source);

#endif   /* FDW_XACT_H */
