/*-------------------------------------------------------------------------
 *
 * fdwxact.c
 *		PostgreSQL distributed transaction manager for foreign servers.
 *
 * To achieve commit among all foreign servers automically, we employee
 * two-phase commit protocol, which is a type of atomic commitment
 * protocol(ACP). The basic strategy is that we prepare all of the remote
 * transactions before committing locally and commit them after committing
 * locally.
 *
 * When a foreign data wrapper starts transaction on a foreign server that
 * is capable of two-phase commit protocol, foreign data wrappers registers
 * the foreign transaction using function FdwXactRegisterForeignTransaction()
 * in order to participate to a group for atomic commit. Participants are
 * identified by oid of foreign server and user. When the foreign transaction
 * begins to modify data the executor marks it as modified using
 * FdwXactMarkForeignTransactionModified().
 *
 * During pre-commit of local transaction, we prepare the transaction on
 * foreign server everywhere. After committing or rolling back locally, we
 * notify the resolver process and tell it to commit or roll back those
 * transactions. If we ask it to commit, we also tell it to notify us when
 * it's done, so that we can wait interruptibly for it to finish, and so
 * that we're not trying to locally do work that might fail when an ERROR
 * after already committed.
 *
 * The best performing way to manage the waiting backends is to have a
 * queue of waiting backends, so that we can avoid searching the through all
 * waiters each time we receive a request. We have two queues: the active
 * queue and the retry queue. The backend is inserted to the active queue at
 * first, and then it is moved to the retry queue by the resolver process if
 * the resolution fails. The backends in the retry queue are processed at
 * interval of foreign_transaction_resolution_retry_interval.
 *
 * Two-phase commit protocol is required if the transaction modified two or more
 * servers including itself. In other case, all foreign transactions are
 * committed during pre-commit.
 *
 * If any network failure, server crash occurs or user stopped waiting
 * prepared foreign transactions are left in in-doubt state (aka. dangling
 * transaction). Dangling transactions are processed by the resolve process
 *
 * During replay WAL and replication FdwXactCtl also holds information about
 * active prepared foreign transaction that haven't been moved to disk yet.
 *
 * Replay of fdwxact records happens by the following rules:
 *
 * 	* On PREPARE redo we add the foreign transaction to FdwXactCtl->fdw_xacts.
 *	  We set fdw_xact->inredo to true for such entries.
 *	* On Checkpoint redo, we iterate through FdwXactCtl->fdw_xacts entries that
 *	  have set fdw_xact->inredo true and are behind the redo_horizon. We save
 *    them to disk and then set fdw_xact->ondisk to true.
 *	* On COMMIT and ABORT we delete the entry from FdwXactCtl->fdw_xacts.
 *	  If fdw_xact->ondisk is true, we delete the corresponding file from
 *	  the disk as well.
 *  * RecoverFdwXacts loads all foreign transaction entries from disk into
 *    memory at server startup.
 *
 * Portions Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/backend/access/fdwxact/fdwxact.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/fdwxact.h"
#include "access/fdwxact_resolver.h"
#include "access/fdwxact_launcher.h"
#include "access/fdwxact_xlog.h"
#include "access/resolver_internal.h"
#include "access/htup_details.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* Is atomic commit requested by user? */
#define IsAtomicCommitEnabled() \
	(max_prepared_foreign_xacts > 0 && \
	 max_foreign_xact_resolvers > 0)

#define IsAtomicCommitRequested() \
	(IsAtomicCommitEnabled() && \
	 (distributed_atomic_commit > DISTRIBUTED_ATOMIC_COMMIT_DISABLED))

/* Structure to bundle the foreign transaction participant */
typedef struct FdwXactParticipant
{
	/*
	 * Pointer to a FdwXact entry in global entry. NULL if
	 * this foreign transaction is registered but not inserted
	 * yet.
	 */
	FdwXact		fdw_xact;
	char		*fdw_xact_id;

	ForeignServer	*server;
	UserMapping		*usermapping;
	bool		modified;					/* true if modified the data on server */
	bool		twophase_commit_enabled;	/* true if the server can execute
											 * two-phase commit protocol */
	void			*fdw_state;				/* fdw-private state */

	/* Callbacks for foreign transaction */
	PrepareForeignTransaction_function	prepare_foreign_xact;
	CommitForeignTransaction_function	commit_foreign_xact;
	RollbackForeignTransaction_function	rollback_foreign_xact;
	GetPrepareId_function				get_prepareid;
	IsTwoPhaseCommitEnabled_function	is_twophase_commit_enabled;
} FdwXactParticipant;

/*
 * List of foreign transaction participants for atomic commit.
 * This list has only foreign servers that support atomic commit FDW
 * API regardless of their configuration.
 */
static List *FdwXactAtomicCommitParticipants = NIL;
static bool FdwXactAtomicCommitReady = false;

/* Directory where the foreign prepared transaction files will reside */
#define FDW_XACTS_DIR "pg_fdw_xact"

/*
 * Name of foreign prepared transaction file is 8 bytes database oid,
 * xid, foreign server oid and user oid separated by '_'.
 *
 * Since FdwXact stat file is created per foreign transaction in a
 * distributed transaction and the xid of unresolved distributed
 * transaction never reused, the name is fairly enough to ensure
 * uniqueness.
 */
#define FDW_XACT_FILE_NAME_LEN (8 + 1 + 8 + 1 + 8 + 1 + 8)
#define FdwXactFilePath(path, dbid, xid, serverid, userid)	\
	snprintf(path, MAXPGPATH, FDW_XACTS_DIR "/%08X_%08X_%08X_%08X", \
			 dbid, xid, serverid, userid)

static FdwXact FdwXactInsertFdwXactEntry(TransactionId xid, FdwXactParticipant *fdw_part);
static void FdwXactPrepareForeignTransactions(void);
static void FdwXactCommitForeignTransaction(FdwXactParticipant *fdw_part);
static bool FdwXactResolveForeignTransaction(FdwXactState *state, FdwXact fdwxact,
											 int elevel);
static void FdwXactComputeRequiredXmin(void);
static bool FdwXactAtomicCommitRequired(void);
static void FdwXactQueueInsert(void);
static void FdwXactCancelWait(void);
static void FdwXactRedoAdd(char *buf, XLogRecPtr start_lsn, XLogRecPtr end_lsn);
static void FdwXactRedoRemove(Oid dbid, TransactionId xid, Oid serverid,
							  Oid userid, bool give_warnings);
static void AtProcExit_FdwXact(int code, Datum arg);
static void ForgetAllFdwXactParticipants(void);
static char *ReadFdwXactFile(Oid dbid, TransactionId xid, Oid serverid,
							 Oid userid, bool give_warnings);
static void RemoveFdwXactFile(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
							  bool giveWarning);
static void RecreateFdwXactFile(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
								void *content, int len);
static void XlogReadFdwXactData(XLogRecPtr lsn, char **buf, int *len);
static char *ProcessFdwXactBuffer(Oid dbid, TransactionId local_xid, Oid serverid,
								  Oid userid, XLogRecPtr insert_start_lsn,
								  bool give_warnings);
static void register_fdw_xact(Oid serverid, Oid userid, bool modified);
static List *get_fdw_xacts(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
						   bool need_lock);
static FdwXact get_one_fdw_xact(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
								bool need_lock);
static FdwXact get_all_fdw_xacts(int *length);
static FdwXact insert_fdw_xact(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
							   Oid umid, char *fdw_xact_id);
static char *generate_fdw_xact_identifier(TransactionId xid, Oid serverid, Oid userid);
static void remove_fdw_xact(FdwXact fdw_xact);
static FdwXactState *create_fdw_xact_state(void);

/* Guc parameters */
int	max_prepared_foreign_xacts = 0;
int	max_foreign_xact_resolvers = 0;
int distributed_atomic_commit = DISTRIBUTED_ATOMIC_COMMIT_DISABLED;

/* Keep track of registering process exit call back. */
static bool fdwXactExitRegistered = false;

/*
 * Remember accessed foreign server. Both RegisterFdwXactByRelId and
 * RegisterFdwXactByServerId are called by executor during initialization.
 */
void
RegisterFdwXactByRelId(Oid relid, bool modified)
{
	Relation		rel;
	Oid				serverid;
	Oid				userid;

	rel = relation_open(relid, NoLock);
	serverid = GetForeignServerIdByRelId(relid);
	userid = rel->rd_rel->relowner ? rel->rd_rel->relowner : GetUserId();
	relation_close(rel, NoLock);

	register_fdw_xact(serverid, userid, modified);
}

void
RegisterFdwXactByServerId(Oid serverid, bool modified)
{
	register_fdw_xact(serverid, GetUserId(), modified);
}

/*
 * Register given foreign transaction identified by given arguments as
 * a participant of the transaction.
 *
 * The foreign server identified by given server id must support atomic
 * commit APIs. Registered foreign transaction are managed by foreign
 * transaction manager until the end of the transaction.
 */
static void
register_fdw_xact(Oid serverid, Oid userid, bool modified)
{
	FdwXactParticipant	*fdw_part;
	ForeignServer 		*foreign_server;
	ForeignDataWrapper	*fdw;
	UserMapping			*user_mapping;
	MemoryContext		old_ctx;
	FdwRoutine			*routine;
	ListCell	   		*lc;

	/*
	 * Participants information is needed at the end of a transaction, where
	 * system cache are not available. Save it in TopTransactionContext
	 * beforehand so that these can live until the end of transaction.
	 */
	old_ctx = MemoryContextSwitchTo(TopTransactionContext);

	routine = GetFdwRoutineByServerId(serverid);

	/*
	 * If the being modified foreign server doesn't have the atomic commit API
	 * we don't manage the foreign transaction in the distributed transaction
	 * manager.
	 */
	if (routine->IsTwoPhaseCommitEnabled == NULL)
	{
		MyXactFlags |= XACT_FLAGS_FDWNOPREPARE;
		pfree(routine);
		return;
	}

	foreach(lc, FdwXactAtomicCommitParticipants)
	{
		FdwXactParticipant	*fp = (FdwXactParticipant *) lfirst(lc);

		if (fp->server->serverid == serverid &&
			fp->usermapping->userid == userid)
		{
			/* The foreign server is already registered, return */
			fp->modified |= modified;
			pfree(routine);
			return;
		}
	}

	foreign_server = GetForeignServer(serverid);
	fdw = GetForeignDataWrapper(foreign_server->fdwid);
	user_mapping = GetUserMapping(userid, serverid);

	/* Make sure that the FDW has transaction handlers */
	if (!routine->PrepareForeignTransaction)
		ereport(ERROR,
				(errmsg("no function provided for preparing foreign transaction for FDW %s",
						fdw->fdwname)));
	if (!routine->CommitForeignTransaction)
		ereport(ERROR,
				(errmsg("no function to commit a foreign transaction provided for FDW %s",
						fdw->fdwname)));
	if (!routine->RollbackForeignTransaction)
		ereport(ERROR,
				(errmsg("no function to rollback a foreign transaction provided for FDW %s",
						fdw->fdwname)));

	fdw_part = (FdwXactParticipant *) palloc(sizeof(FdwXactParticipant));

	fdw_part->fdw_xact_id = NULL;
	fdw_part->server = foreign_server;
	fdw_part->usermapping = user_mapping;
	fdw_part->fdw_xact = NULL;
	fdw_part->modified = modified;
	fdw_part->twophase_commit_enabled = true; /* by default, will be changed at pre-commit phase */
	fdw_part->fdw_state = NULL;
	fdw_part->prepare_foreign_xact = routine->PrepareForeignTransaction;
	fdw_part->commit_foreign_xact = routine->CommitForeignTransaction;
	fdw_part->rollback_foreign_xact = routine->RollbackForeignTransaction;
	fdw_part->is_twophase_commit_enabled = routine->IsTwoPhaseCommitEnabled;
	fdw_part->get_prepareid = routine->GetPrepareId;

	/* Add this foreign transaction to the participants list */
	FdwXactAtomicCommitParticipants = lappend(FdwXactAtomicCommitParticipants, fdw_part);

	/* Revert back the context */
	MemoryContextSwitchTo(old_ctx);
}

/*
 * FdwXactShmemSize
 * Calculates the size of shared memory allocated for maintaining foreign
 * prepared transaction entries.
 */
Size
FdwXactShmemSize(void)
{
	Size		size;

	/* Size for foreign transaction information array */
	size = offsetof(FdwXactCtlData, fdw_xacts);
	size = add_size(size, mul_size(max_prepared_foreign_xacts,
								   sizeof(FdwXact)));
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_prepared_foreign_xacts,
								   sizeof(FdwXactData)));

	return size;
}

/*
 * FdwXactShmemInit
 * Initialization of shared memory for maintaining foreign prepared transaction
 * entries. The shared memory layout is defined in definition of FdwXactCtlData
 * structure.
 */
void
FdwXactShmemInit(void)
{
	bool		found;

	if (!fdwXactExitRegistered)
	{
		before_shmem_exit(AtProcExit_FdwXact, 0);
		fdwXactExitRegistered = true;
	}

	FdwXactCtl = ShmemInitStruct("Foreign transactions table",
								 FdwXactShmemSize(),
								 &found);
	if (!IsUnderPostmaster)
	{
		FdwXact		fdw_xacts;
		int			cnt;

		Assert(!found);
		FdwXactCtl->freeFdwXacts = NULL;
		FdwXactCtl->numFdwXacts = 0;

		/* Initialize the linked list of free FDW transactions */
		fdw_xacts = (FdwXact)
			((char *) FdwXactCtl +
			 MAXALIGN(offsetof(FdwXactCtlData, fdw_xacts) +
					  sizeof(FdwXact) * max_prepared_foreign_xacts));
		for (cnt = 0; cnt < max_prepared_foreign_xacts; cnt++)
		{
			fdw_xacts[cnt].status = FDW_XACT_INITIAL;
			fdw_xacts[cnt].fxact_free_next = FdwXactCtl->freeFdwXacts;
			FdwXactCtl->freeFdwXacts = &fdw_xacts[cnt];
		}
	}
	else
	{
		Assert(FdwXactCtl);
		Assert(found);
	}
}

/*
 * PreCommit_FdwXacts
 *
 */
void
PreCommit_FdwXacts(void)
{
	bool		need_atomic_commit;
	ListCell	*lc;
	ListCell	*next;
	ListCell	*prev = NULL;

	/* If there are no foreign servers involved, we have no business here */
	if (FdwXactAtomicCommitParticipants == NIL)
		return;

	need_atomic_commit = FdwXactAtomicCommitRequired();

	/*
	 * If 'require' case, we require all modified server have to be capable of
	 * two-phase commit protocol.
	 */
	if (need_atomic_commit &&
		distributed_atomic_commit == DISTRIBUTED_ATOMIC_COMMIT_REQUIRED &&
		(MyXactFlags & XACT_FLAGS_FDWNOPREPARE) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot COMMIT a distributed transaction that has operated on foreign server that doesn't support atomic commit")));

	/*
	 * Commit transactions on foreign servers.
	 *
	 * Committed transactions are removed from FdwXactAtomicCommitParticipants
	 * so that the later preparation can process only servers that requires to be commit
	 * using two-phase commit protocol.
	 */
	for (lc = list_head(FdwXactAtomicCommitParticipants); lc != NULL; lc = next)
	{
		FdwXactParticipant *fdw_part = (FdwXactParticipant *) lfirst(lc);
		bool can_commit = false;

		next = lnext(lc);

		if (!need_atomic_commit || !fdw_part->modified)
		{
			/*
			 * We can commit not-modified servers and when the atomic commit is not
			 * required.
			 */
			can_commit = true;
		}
		else if (distributed_atomic_commit == DISTRIBUTED_ATOMIC_COMMIT_PREFER &&
				 !fdw_part->twophase_commit_enabled)
		{
			/* Also in 'prefer' case, non-2pc-capable servers can be committed */
			can_commit = true;
		}

		if (can_commit)
		{
			/* Commit the foreign transaction */
			FdwXactCommitForeignTransaction(fdw_part);

			/* Delete it from the participant list */
			FdwXactAtomicCommitParticipants =
				list_delete_cell(FdwXactAtomicCommitParticipants, lc, prev);

			continue;
		}

		prev = lc;
	}

	/*
	 * If only one participant of all participants is modified, we can commit it.
	 * This can avoid to use two-phase commit for only one server in the 'prefer' case
	 * where the transaction has one 2pc-capable modified server and some modified
	 * servers.
	 */
	if (list_length(FdwXactAtomicCommitParticipants) == 1 &&
		(MyXactFlags & XACT_FLAGS_WROTENONTEMPREL) == 0)
	{
		Assert(distributed_atomic_commit == DISTRIBUTED_ATOMIC_COMMIT_PREFER);
		FdwXactCommitForeignTransaction(linitial(FdwXactAtomicCommitParticipants));
		list_free(FdwXactAtomicCommitParticipants);
		return;
	}

	FdwXactPrepareForeignTransactions();
	/* keep FdwXactparticipantsForAC until the end of transaction */
}

/*
 * FdwXactPrepareForeignTransactions
 *
 * Prepare all foreign transaction participants.  This function creates a prepared
 * participants chain each time when we prepared a foreign transaction. The prepared
 * participants chain is used to access all participants of distributed transaction
 * quickly. If any one of them fails to prepare, we change over aborts.
 */
static void
FdwXactPrepareForeignTransactions(void)
{
	FdwXactState *state;
	ListCell   *lcell;
	FdwXact		prev_fdwxact = NULL;
	TransactionId txid;

	if (FdwXactAtomicCommitParticipants == NIL)
		return;

	/* Parameter check */
	if (max_prepared_foreign_xacts == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("prepread foreign transactions are disabled"),
				 errhint("Set max_prepared_foreign_transactions to a nonzero value.")));

	if (max_foreign_xact_resolvers == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("prepread foreign transactions are disabled"),
				 errhint("Set max_foreign_transaction_resolvers to a nonzero value.")));

	state = create_fdw_xact_state();

	/* Loop over the foreign connections */
	txid = GetTopTransactionId();
	foreach(lcell, FdwXactAtomicCommitParticipants)
	{
		FdwXactParticipant *fdw_part = (FdwXactParticipant *) lfirst(lcell);
		FdwXact		fdwxact;

		/* Generate an unique identifier */
		if (fdw_part->get_prepareid)
		{
			char *id;
			int fdwxact_id_len = 0;

			id = fdw_part->get_prepareid(txid, fdw_part->server->serverid,
										 fdw_part->usermapping->userid,
										 &fdwxact_id_len);

			if (!id)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 (errmsg("foreign transaction identifier is not provided"))));

			/* Check length of foreign transaction identifier */
			id[fdwxact_id_len] = '\0';
			if (fdwxact_id_len > NAMEDATALEN)
				ereport(ERROR,
						(errcode(ERRCODE_NAME_TOO_LONG),
						 errmsg("foreign transaction identifer \"%s\" is too long",
								id),
						 errdetail("foreign transaction identifier must be less than %d characters.",
								   NAMEDATALEN)));

			fdw_part->fdw_xact_id = pstrdup(id);
		}
		else
			fdw_part->fdw_xact_id = generate_fdw_xact_identifier(txid,
																 fdw_part->server->serverid,
																 fdw_part->usermapping->userid);

		/*
		 * Insert the foreign transaction entry. Registration persists this
		 * information to the disk and logs (that way relaying it on standby).
		 * Thus in case we loose connectivity to the foreign server or crash
		 * ourselves, we will remember that we might have prepared transaction
		 * on the foreign server and try to resolve it when connectivity is
		 * restored or after crash recovery.
		 *
		 * If we prepare the transaction on the foreign server before persisting
		 * the information to the disk and crash in-between these two steps,
		 * we will forget that we prepared the transaction on the foreign server
		 * and will not be able to resolve it after the crash. Hence persist
		 * first then prepare.
		 */
		fdwxact = FdwXactInsertFdwXactEntry(txid, fdw_part);

		state->serverid = fdw_part->server->serverid;
		state->userid = fdw_part->usermapping->userid;
		state->umid = fdw_part->usermapping->umid;
		state->fdwxact_id = pstrdup(fdwxact->fdw_xact_id);

		/*
		 * Between FdwXactInsertFdwXactEntry call till this backend hears
		 * acknowledge from foreign server, the backend may abort the local
		 * transaction (say, because of a signal). During abort processing,
		 * we might try to resolve a never-prepared transaction, and get an error.
		 * This is fine as long as the FDW provides us unique prepared transaction
		 * identifiers.
		 */
		if (!fdw_part->prepare_foreign_xact(state))
		{
			/* Failed to prepare, change over aborts */
			ereport(ERROR,
					(errmsg("could not prepare transaction on foreign server %s",
							fdw_part->server->servername)));
		}

		/* Keep fdw_state until end of transaction */
		fdw_part->fdw_state = state->fdw_state;

		/* Preparation is success, update its status */
		LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
		fdw_part->fdw_xact->status = FDW_XACT_PREPARED;
		fdw_part->fdw_xact = fdwxact;
		LWLockRelease(FdwXactLock);

		/*
		 * Create a prepared participants chain, which is link-ed FdwXact entries
		 * involving with this transaction.
		 */
		if (prev_fdwxact)
		{
			/* Append others to the tail */
			Assert(fdwxact->fxact_next == NULL);
			prev_fdwxact->fxact_next = fdwxact;
		}
	}
}

/*
 * Commit the given foreign transaction.
 */
void
FdwXactCommitForeignTransaction(FdwXactParticipant *fdw_part)
{
	FdwXactState *state;

	state = create_fdw_xact_state();
	state->serverid = fdw_part->server->serverid;
	state->userid = fdw_part->usermapping->userid;
	state->umid = fdw_part->usermapping->umid;
	fdw_part->fdw_state = (void *) state;

	if (!fdw_part->commit_foreign_xact(state))
		ereport(ERROR,
				(errmsg("could not commit foreign transaction on server %s",
						fdw_part->server->servername)));
}

/*
 * FdwXactInsertFdwXactEntry
 *
 * This function is used to create new foreign transaction entry before an FDW
 * prepares and commit/rollback. The function adds the entry to WAL and it will
 * be persisted to the disk under pg_fdw_xact directory when checkpoint.
 */
static FdwXact
FdwXactInsertFdwXactEntry(TransactionId xid, FdwXactParticipant *fdw_part)
{
	FdwXact				fxact;
	FdwXactOnDiskData	*fxact_file_data;
	MemoryContext		old_context;
	int					data_len;

	old_context = MemoryContextSwitchTo(TopTransactionContext);

	/*
	 * Enter the foreign transaction in the shared memory structure.
	 */
	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	fxact = insert_fdw_xact(MyDatabaseId, xid, fdw_part->server->serverid,
							fdw_part->usermapping->userid,
							fdw_part->usermapping->umid, fdw_part->fdw_xact_id);
	fxact->status = FDW_XACT_PREPARING;
	fxact->held_by = MyBackendId;
	fdw_part->fdw_xact = fxact;
	LWLockRelease(FdwXactLock);

	MemoryContextSwitchTo(old_context);

	/*
	 * Prepare to write the entry to a file. Also add xlog entry. The contents
	 * of the xlog record are same as what is written to the file.
	 */
	data_len = offsetof(FdwXactOnDiskData, fdw_xact_id);
	data_len = data_len + strlen(fdw_part->fdw_xact_id) + 1;
	data_len = MAXALIGN(data_len);
	fxact_file_data = (FdwXactOnDiskData *) palloc0(data_len);
	fxact_file_data->dbid = MyDatabaseId;
	fxact_file_data->local_xid = xid;
	fxact_file_data->serverid = fdw_part->server->serverid;
	fxact_file_data->userid = fdw_part->usermapping->userid;
	fxact_file_data->umid = fdw_part->usermapping->umid;
	memcpy(fxact_file_data->fdw_xact_id, fdw_part->fdw_xact_id,
		   strlen(fdw_part->fdw_xact_id) + 1);

	/* See note in RecordTransactionCommit */
	MyPgXact->delayChkpt = true;

	START_CRIT_SECTION();

	/* Add the entry in the xlog and save LSN for checkpointer */
	XLogBeginInsert();
	XLogRegisterData((char *) fxact_file_data, data_len);
	fxact->insert_end_lsn = XLogInsert(RM_FDW_XACT_ID, XLOG_FDW_XACT_INSERT);
	XLogFlush(fxact->insert_end_lsn);

	/* If we crash now, we have prepared: WAL replay will fix things */

	/* Store record's start location to read that later on CheckPoint */
	fxact->insert_start_lsn = ProcLastRecPtr;

	/* File is written completely, checkpoint can proceed with syncing */
	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	fxact->valid = true;
	LWLockRelease(FdwXactLock);

	/* Checkpoint can process now */
	MyPgXact->delayChkpt = false;

	END_CRIT_SECTION();

	pfree(fxact_file_data);
	return fxact;
}

/*
 * insert_fdw_xact
 *
 * Insert a new entry for a given foreign transaction identified by transaction
 * id, foreign server and user mapping, into the shared memory array. Caller
 * must hold FdwXactLock in exclusive mode.
 *
 * If the entry already exists, the function raises an error.
 */
static FdwXact
insert_fdw_xact(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
				Oid umid, char *fdw_xact_id)
{
	int i;
	FdwXact fxact;

	Assert(LWLockHeldByMeInMode(FdwXactLock, LW_EXCLUSIVE));

	/* Check for duplicated foreign transaction entry */
	for (i = 0; i < FdwXactCtl->numFdwXacts; i++)
	{
		fxact = FdwXactCtl->fdw_xacts[i];
		if (fxact->dbid == dbid &&
			fxact->local_xid == xid &&
			fxact->serverid == serverid &&
			fxact->userid == userid)
			ereport(ERROR, (errmsg("could not insert a foreign transaction entry"),
							errdetail("duplicate entry with transaction id %u, serverid %u, userid %u",
								   xid, serverid, userid)));
	}

	/*
	 * Get a next free foreign transaction entry. Raise error if there are
	 * none left.
	 */
	if (!FdwXactCtl->freeFdwXacts)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of foreign transactions reached"),
				 errhint("Increase max_prepared_foreign_transactions: \"%d\".",
						 max_prepared_foreign_xacts)));
	}
	fxact = FdwXactCtl->freeFdwXacts;
	FdwXactCtl->freeFdwXacts = fxact->fxact_free_next;

	/* Insert the entry to shared memory array */
	Assert(FdwXactCtl->numFdwXacts < max_prepared_foreign_xacts);
	FdwXactCtl->fdw_xacts[FdwXactCtl->numFdwXacts++] = fxact;

	fxact->held_by = InvalidBackendId;
	fxact->dbid = dbid;
	fxact->local_xid = xid;
	fxact->serverid = serverid;
	fxact->userid = userid;
	fxact->umid = umid;
	fxact->insert_start_lsn = InvalidXLogRecPtr;
	fxact->insert_end_lsn = InvalidXLogRecPtr;
	fxact->valid = false;
	fxact->ondisk = false;
	fxact->inredo = false;
	memcpy(fxact->fdw_xact_id, fdw_xact_id, strlen(fdw_xact_id) + 1);

	return fxact;
}

/*
 * remove_fdw_xact
 *
 * Remove the foreign prepared transaction entry from shared memory.
 * Caller must hold FdwXactLock in exclusive mode.
 */
static void
remove_fdw_xact(FdwXact fdw_xact)
{
	int			cnt;

	Assert(fdw_xact != NULL);
	Assert(LWLockHeldByMeInMode(FdwXactLock, LW_EXCLUSIVE));

	/* Search the slot where this entry resided */
	for (cnt = 0; cnt < FdwXactCtl->numFdwXacts; cnt++)
	{
		if (FdwXactCtl->fdw_xacts[cnt] == fdw_xact)
			break;
	}

	/* We did not find the given entry in the array */
	if (cnt >= FdwXactCtl->numFdwXacts)
		ereport(ERROR,
				(errmsg("could not remove a foreign transaction entry"),
				 errdetail("failed to find entry for xid %u, foreign server %u, and user %u",
						   fdw_xact->local_xid, fdw_xact->serverid, fdw_xact->userid)));

	/* Remove the entry from active array */
	FdwXactCtl->numFdwXacts--;
	FdwXactCtl->fdw_xacts[cnt] = FdwXactCtl->fdw_xacts[FdwXactCtl->numFdwXacts];

	/* Put it back into free list */
	fdw_xact->fxact_free_next = FdwXactCtl->freeFdwXacts;
	FdwXactCtl->freeFdwXacts = fdw_xact;

	/* Reset informations */
	fdw_xact->status = FDW_XACT_INITIAL;
	fdw_xact->held_by = InvalidBackendId;
	fdw_xact->fxact_next = NULL;

	if (!RecoveryInProgress())
	{
		xl_fdw_xact_remove record;
		XLogRecPtr	recptr;

		/* Fill up the log record before releasing the entry */
		record.serverid = fdw_xact->serverid;
		record.dbid = fdw_xact->dbid;
		record.xid = fdw_xact->local_xid;
		record.userid = fdw_xact->userid;

		/*
		 * Now writing FdwXact state data to WAL. We have to set delayChkpt
		 * here, otherwise a checkpoint starting immediately after the
		 * WAL record is inserted could complete without fsync'ing our
		 * state file.  (This is essentially the same kind of race condition
		 * as the COMMIT-to-clog-write case that RecordTransactionCommit
		 * uses delayChkpt for; see notes there.)
		 */
		START_CRIT_SECTION();

		MyPgXact->delayChkpt = true;

		/*
		 * Log that we are removing the foreign transaction entry and
		 * remove the file from the disk as well.
		 */
		XLogBeginInsert();
		XLogRegisterData((char *) &record, sizeof(xl_fdw_xact_remove));
		recptr = XLogInsert(RM_FDW_XACT_ID, XLOG_FDW_XACT_REMOVE);
		XLogFlush(recptr);

		/*
		 * Now we can mark ourselves as out of the commit critical section: a
		 * checkpoint starting after this will certainly see the gxact as a
		 * candidate for fsyncing.
		 */
		MyPgXact->delayChkpt = false;

		END_CRIT_SECTION();
	}
}

/*
 * Return true and set FdwXactAtomicCommitReady to true if we require atomic commit.
 * It is required if the transaction modified data on two or more servers including
 * local node itself. This function also checks for each server if two-phase commit
 * is enabled or not.
 */
static bool
FdwXactAtomicCommitRequired(void)
{
	ListCell*	lc;
	int			nserverswritten = 0;

	if (!IsAtomicCommitRequested())
		return false;

	foreach(lc, FdwXactAtomicCommitParticipants)
	{
		FdwXactParticipant *fdw_part = (FdwXactParticipant *) lfirst(lc);

		/* Check if the foreign server is capable of two-phase commit protocol */
		if (fdw_part->is_twophase_commit_enabled(fdw_part->server->serverid))
			fdw_part->twophase_commit_enabled = true;
		else if (fdw_part->modified)
			MyXactFlags |= XACT_FLAGS_FDWNOPREPARE;

		if (fdw_part->modified)
			nserverswritten++;
	}

	if ((MyXactFlags & XACT_FLAGS_WROTENONTEMPREL) != 0)
		++nserverswritten;

	/* Atomic commit is required if we modified data on two or more participants */
	if (nserverswritten <= 1)
		return false;

	FdwXactAtomicCommitReady = true;
	return true;
}

bool
FdwXactIsAtomicCommitReady(void)
{
	return FdwXactAtomicCommitReady;
}

/*
 * Compute the oldest xmin across all unresolved foreign transactions
 * and store it in the ProcArray.
 */
static void
FdwXactComputeRequiredXmin(void)
{
	int	i;
	TransactionId agg_xmin = InvalidTransactionId;

	Assert(FdwXactCtl != NULL);

	LWLockAcquire(FdwXactLock, LW_SHARED);

	for (i = 0; i < FdwXactCtl->numFdwXacts; i++)
	{
		FdwXact fdwxact = FdwXactCtl->fdw_xacts[i];

		if (!fdwxact->valid)
			continue;

		Assert(TransactionIdIsValid(fdwxact->local_xid));

		if (!TransactionIdIsValid(agg_xmin) ||
			TransactionIdPrecedes(fdwxact->local_xid, agg_xmin))
			agg_xmin = fdwxact->local_xid;
	}

	LWLockRelease(FdwXactLock);

	ProcArraySetFdwXactUnresolvedXmin(agg_xmin);
}

/*
 * ForgetAllFdwXactParticipants
 *
 * Reset all the foreign transaction entries that this backend registered.
 * If the foreign transaction has the corresponding FdwXact entry, resetting
 * the held_by field means to leave that entry in unresolved state. If we
 * leaves any entries, we update the oldest xmin of unresolved transaction
 * so that transaction status of dangling transaction are not truncated.
 */
static void
ForgetAllFdwXactParticipants(void)
{
	ListCell *cell;
	int		n_lefts = 0;

	if (FdwXactAtomicCommitParticipants == NIL)
		return;

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);

	foreach(cell, FdwXactAtomicCommitParticipants)
	{
		FdwXactParticipant	*fdw_part = (FdwXactParticipant *) lfirst(cell);

		/* Skip if didn't register FdwXact entry yet */
		if (fdw_part->fdw_xact == NULL)
			continue;

		/*
		 * There is a race condition; the FdwXact entries in
		 * FdwXactAtomicCommitParticipants could be used by other backend before we
		 * forget in case where the resolver process removes the FdwXact entry
		 * and other backend reuses it before we forget. So we need to check
		 * if the entries are still associated with the transaction.
		 */
		if (fdw_part->fdw_xact->held_by == MyBackendId)
		{
			fdw_part->fdw_xact->held_by = InvalidBackendId;
			n_lefts++;
		}
	}

	LWLockRelease(FdwXactLock);

	/*
	 * Update the oldest local transaction of unresolved distributed
	 * transaction if we leaved any FdwXact entries.
	 */
	if (n_lefts > 0)
		FdwXactComputeRequiredXmin();

	FdwXactAtomicCommitParticipants = NIL;
}

/*
 * AtProcExit_FdwXact
 *
 * When the process exits, forget all the entries.
 */
static void
AtProcExit_FdwXact(int code, Datum arg)
{
	ForgetAllFdwXactParticipants();
}

/*
 * Wait for foreign transaction to be resolved.
 *
 * Initially backends start in state FDW_XACT_NOT_WAITING and then change
 * that state to FDW_XACT_WAITING before adding ourselves to the wait queue.
 * During FdwXactResolveForeignTransaction a fdwxact resolver changes the
 * state to FDW_XACT_WAIT_COMPLETE once foreign transactions are resolved.
 * This backend then resets its state to FDW_XACT_NOT_WAITING.
 * If a resolver fails to resolve the waiting transaction it moves us to
 * the retry queue and changes the state to FDW_XACT_WAITING_RETRY.
 *
 * This function is inspired by SyncRepWaitForLSN.
 */
void
FdwXactWaitToBeResolved(TransactionId wait_xid, bool is_commit)
{
	char		*new_status = NULL;
	const char	*old_status;
	ListCell	*lc;
	List		*fdwxact_participants = NIL;

	/* Quick exit if atomic commit is not requested */
	if (!IsAtomicCommitRequested())
		return;

	Assert(FdwXactCtl != NULL);
	Assert(TransactionIdIsValid(wait_xid));
	Assert(SHMQueueIsDetached(&(MyProc->fdwXactLinks)));
	Assert(MyProc->fdwXactState == FDW_XACT_NOT_WAITING);

	if (FdwXactAtomicCommitParticipants != NIL)
	{
		/*
		 * If we're waiting for foreign transactions to be resolved that
		 * we've prepared just before, use the participants list.
		 */
		Assert(MyPgXact->xid == wait_xid);
		fdwxact_participants = FdwXactAtomicCommitParticipants;
	}
	else
	{
		/*
		 * Get participants list from the global array. This is required (1)
		 * when we're waiting for foreign transactions to be resolved that
		 * is part of a local prepared transaction that is marked as prepared
		 * during running, or (2) when we resolve the PREPARE'd distributed
		 * transaction after restart.
		 */
		fdwxact_participants = get_fdw_xacts(MyDatabaseId, wait_xid,
											 InvalidOid, InvalidOid, true);
	}

	/* Exit if we found no foreign transaction to resolve */
	if (fdwxact_participants == NIL)
		return;

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);

	foreach(lc, fdwxact_participants)
	{
		FdwXact fdw_xact = (FdwXact) lfirst(lc);

		/* Don't overwrite status if fate has been determined */
		if (fdw_xact->status == FDW_XACT_PREPARED)
			fdw_xact->status = (is_commit ?
								FDW_XACT_COMMITTING_PREPARED :
								FDW_XACT_ABORTING_PREPARED);
	}

	/* Set backend status and enqueue itself to the active queue*/
	MyProc->fdwXactState = FDW_XACT_WAITING;
	MyProc->fdwXactWaitXid = wait_xid;
	FdwXactQueueInsert();
	LWLockRelease(FdwXactLock);

	/* Launch a resolver process if not yet, or wake it up */
	fdwxact_maybe_launch_resolver(false);

	/*
	 * Alter ps display to show waiting for foreign transaction
	 * resolution.
	 */
	if (update_process_title)
	{
		int len;

		old_status = get_ps_display(&len);
		new_status = (char *) palloc(len + 31 + 1);
		memcpy(new_status, old_status, len);
		sprintf(new_status + len, " waiting for resolution %d", wait_xid);
		set_ps_display(new_status, false);
		new_status[len] = '\0';	/* truncate off "waiting ..." */
	}

	/* Wait for all foreign transactions to be resolved */
	for (;;)
	{
		/* Must reset the latch before testing state */
		ResetLatch(MyLatch);

		/*
		 * Acquiring the lock is not needed, the latch ensures proper
		 * barriers. If it looks like we're done, we must really be done,
		 * because once walsender changes the state to FDW_XACT_WAIT_COMPLETE,
		 * it will never update it again, so we can't be seeing a stale value
		 * in that case.
		 */
		if (MyProc->fdwXactState == FDW_XACT_WAIT_COMPLETE)
			break;

		/*
		 * If a wait for foreign transaction resolution is pending, we can
		 * neither acknowledge the commit nor raise ERROR or FATAL.  The latter
		 * would lead the client to believe that the distributed transaction
		 * aborted, which is not true: it's already committed locally. The
		 * former is no good either: the client has requested committing a
		 * distributed transaction, and is entitled to assume that a acknowledged
		 * commit is also commit on all foreign servers, which might not be
		 * true. So in this case we issue a WARNING (which some clients may
		 * be able to interpret) and shut off further output. We do NOT reset
		 * PorcDiePending, so that the process will die after the commit is
		 * cleaned up.
		 */
		if (ProcDiePending)
		{
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("canceling the wait for resolving foreign transaction and terminating connection due to administrator command"),
					 errdetail("The transaction has already committed locally, but might not have been committed on the foreign server.")));
			whereToSendOutput = DestNone;
			FdwXactCancelWait();
			break;
		}

		/*
		 * If a query cancel interrupt arrives we just terminate the wait with
		 * a suitable warning. The foreign transactions can be orphaned but
		 * the foreign xact resolver can pick up them and tries to resolve them
		 * later.
		 */
		if (QueryCancelPending)
		{
			QueryCancelPending = false;
			ereport(WARNING,
					(errmsg("canceling wait for resolving foreign transaction due to user request"),
					 errdetail("The transaction has already committed locally, but might not have been committed on the foreign server.")));
			FdwXactCancelWait();
			break;
		}

		/*
		 * If the postmaster dies, we'll probably never get an
		 * acknowledgement, because all the wal sender processes will exit. So
		 * just bail out.
		 */
		if (!PostmasterIsAlive())
		{
			ProcDiePending = true;
			whereToSendOutput = DestNone;
			FdwXactCancelWait();
			break;
		}

		/*
		 * Wait on latch.  Any condition that should wake us up will set the
		 * latch, so no need for timeout.
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1,
				  WAIT_EVENT_FDW_XACT_RESOLUTION);
	}

	pg_read_barrier();

	Assert(SHMQueueIsDetached(&(MyProc->fdwXactLinks)));
	MyProc->fdwXactState = FDW_XACT_NOT_WAITING;

	/*
	 * Forget the list of locked entries, also means that the entries
	 * that could not resolved are remained as dangling transactions.
	 */
	ForgetAllFdwXactParticipants();

	if (new_status)
	{
		set_ps_display(new_status, false);
		pfree(new_status);
	}
}

/*
 * Acquire FdwXactLock and cancel any wait currently in progress.
 */
static void
FdwXactCancelWait(void)
{
	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	if (!SHMQueueIsDetached(&(MyProc->fdwXactLinks)))
		SHMQueueDelete(&(MyProc->fdwXactLinks));
	MyProc->fdwXactState = FDW_XACT_NOT_WAITING;
	LWLockRelease(FdwXactLock);
}

/*
 * Insert MyProc into the tail of FdwXactActiveQueue.
 */
static void
FdwXactQueueInsert(void)
{
	SHMQueueInsertBefore(&(FdwXactRslvCtl->FdwXactActiveQueue),
						 &(MyProc->fdwXactLinks));
}

void
FdwXactCleanupAtProcExit(void)
{
	if (!SHMQueueIsDetached(&(MyProc->fdwXactLinks)))
	{
		LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
		SHMQueueDelete(&(MyProc->fdwXactLinks));
		LWLockRelease(FdwXactLock);
	}
}

/*
 * Resolve one distributed transaction. The target distributed transaction
 * is fetched from either the active queue or the retry queue and its participants
 * are fetched from either the global array.
 *
 * Release the waiter and return true if we resolved the all of the foreign
 * transaction participants. On failure, we move the FdwXactLinks entry to the
 * retry queue from the active queue, and raise an error and exit.
 */
bool
FdwXactResolveDistributedTransaction(Oid dbid, bool is_active)
{
	FdwXactState	*state;
	ListCell		*lc;
	ListCell		*next;
	PGPROC			*waiter = NULL;
	List			*participants;
	SHM_QUEUE		*target_queue;

	if (is_active)
		target_queue = &(FdwXactRslvCtl->FdwXactActiveQueue);
	else
		target_queue = &(FdwXactRslvCtl->FdwXactRetryQueue);

	LWLockAcquire(FdwXactLock, LW_SHARED);

	/* Fetch a waiter from beginning of the queue */
	while ((waiter = (PGPROC *) SHMQueueNext(target_queue, target_queue,
											 offsetof(PGPROC, fdwXactLinks))) != NULL)
	{
		/* Found a waiter */
		if (waiter->databaseId == dbid)
			break;
	}

	/* If no waiter, there is no job */
	if (!waiter)
	{
		LWLockRelease(FdwXactLock);
		return false;
	}

	Assert(TransactionIdIsValid(waiter->fdwXactWaitXid));

	state = create_fdw_xact_state();
	participants = get_fdw_xacts(dbid, waiter->fdwXactWaitXid, InvalidOid,
								 InvalidOid, false);
	LWLockRelease(FdwXactLock);

	/* Resolve all foreign transactions one by one */
	for (lc = list_head(participants); lc != NULL; lc = next)
	{
		FdwXact fdwxact = (FdwXact) lfirst(lc);

		CHECK_FOR_INTERRUPTS();

		next = lnext(lc);

		state->serverid = fdwxact->serverid;
		state->userid = fdwxact->userid;
		state->umid = fdwxact->umid;
		state->fdwxact_id = pstrdup(fdwxact->fdw_xact_id);

		PG_TRY();
		{
			FdwXactResolveForeignTransaction(state, fdwxact, ERROR);
		}
		PG_CATCH();
		{
			/* Re-insert the waiter to the retry queue */
			LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
			if (waiter->fdwXactState == FDW_XACT_WAITING)
			{
				SHMQueueDelete(&(waiter->fdwXactLinks));
				pg_write_barrier();
				SHMQueueInsertBefore(&(FdwXactRslvCtl->FdwXactRetryQueue),
									 &(waiter->fdwXactLinks));
				waiter->fdwXactState = FDW_XACT_WAITING_RETRY;
			}
			LWLockRelease(FdwXactLock);

			PG_RE_THROW();
		}
		PG_END_TRY();

		elog(DEBUG2, "resolved a foreign transaction xid %u, serverid %d, userid %d",
			 fdwxact->local_xid, fdwxact->serverid, fdwxact->userid);
	}

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);

	/*
	 * Remove waiter from shmem queue, if not detached yet. The waiter
	 * could already be detached if user cancelled to wait before
	 * resolution.
	 */
	if (!SHMQueueIsDetached(&(waiter->fdwXactLinks)))
	{
		TransactionId	wait_xid = waiter->fdwXactWaitXid;

		SHMQueueDelete(&(waiter->fdwXactLinks));
		pg_write_barrier();

		/* Set state to complete */
		waiter->fdwXactState = FDW_XACT_WAIT_COMPLETE;

		/* Wake up the waiter only when we have set state and removed from queue */
		SetLatch(&(waiter->procLatch));

		elog(DEBUG2, "released the proc xid %u", wait_xid);
	}

	LWLockRelease(FdwXactLock);

	return true;
}

/*
 * Resolve all dangling foreign transactions on the given database. Get
 * all dangling foreign transactions from shmem global array and resolve
 * them one by one.
 */
void
FdwXactResolveAllDanglingTransactions(Oid dbid)
{
	List		*dangling_fdwxacts = NIL;
	ListCell	*cell;
	bool		n_resolved = 0;
	int			i;

	Assert(OidIsValid(dbid));

	LWLockAcquire(FdwXactLock, LW_SHARED);

	/*
	 * Walk over the global array to make the list of dangling transactions
	 * of which corresponding local transaction is on the given database.
	 */
	for (i = 0; i < FdwXactCtl->numFdwXacts; i++)
	{
		FdwXact fxact = FdwXactCtl->fdw_xacts[i];

		/*
		 * Append the fdwxact entry on the given database to the list if
		 * it's handled by nobody and the corresponding local transaction
		 * is not part of the prepared transaction.
		 */
		if (fxact->dbid == dbid &&
			fxact->held_by == InvalidBackendId &&
			!TwoPhaseExists(fxact->local_xid))
			dangling_fdwxacts = lappend(dangling_fdwxacts, fxact);
	}

	LWLockRelease(FdwXactLock);

	/* Return if there is no foreign transaction we need to resolve */
	if (dangling_fdwxacts == NIL)
		return;

	foreach(cell, dangling_fdwxacts)
	{
		FdwXact fdwxact = (FdwXact) lfirst(cell);
		FdwXactState *state;

		state = create_fdw_xact_state();
		state->serverid = fdwxact->serverid;
		state->userid = fdwxact->userid;
		state->umid = fdwxact->umid;
		state->fdwxact_id = pstrdup(fdwxact->fdw_xact_id);

		FdwXactResolveForeignTransaction(state, fdwxact, ERROR);

		n_resolved++;
	}

	list_free(dangling_fdwxacts);

	elog(DEBUG2, "resolved %d dangling foreign xacts", n_resolved);
}

/*
 * AtEOXact_FdwXacts
 *
 * In commit case, we have already prepared transactions on the foreign
 * servers during pre-commit. And that prepared transactions will be
 * resolved by the resolver process. So we don't do anything about the
 * foreign transaction.
 *
 * In abort case, user requested rollback or we changed over rollback
 * due to error during commit. To close current foreign transaction anyway
 * we call rollback API to every foreign transaction. If we raised an error
 * during preparing and came to here, it's possible that some entries of
 * FdwXactParticipants already registered its FdwXact entry. If there is
 * we leave them as dangling transaction and ask the resolver process to
 * process them.
 */
extern void
AtEOXact_FdwXacts(bool is_commit)
{
	ListCell   *lcell;

	if (!is_commit)
	{
		int left_fdwxacts = 0;
		FdwXactState *state = create_fdw_xact_state();

		foreach (lcell, FdwXactAtomicCommitParticipants)
		{
			FdwXactParticipant	*fdw_part = lfirst(lcell);

			/*
			 * Count FdwXact entries that we registered to shared memory array
			 * in this transaction.
			 */
			if (fdw_part->fdw_xact)
			{
				/*
				 * The status of foreign transaction must be either preparing
				 * or prepared. In any case, since we have registered FdwXact
				 * entry we leave them to the resolver process. For the preparing
				 * state, since the foreign transaction might not close yet we
				 * fall through and call rollback API. For the prepared state,
				 * since the foreign transaction has closed we don't need to do
				 * anything.
				 */
				Assert(fdw_part->fdw_xact->status == FDW_XACT_PREPARING ||
					   fdw_part->fdw_xact->status == FDW_XACT_PREPARED);

				left_fdwxacts++;
				if (fdw_part->fdw_xact->status == FDW_XACT_PREPARED)
					continue;
			}

			state->serverid = fdw_part->server->serverid;
			state->userid = fdw_part->usermapping->userid;
			state->umid = fdw_part->usermapping->umid;
			state->fdw_state = fdw_part->fdw_state;

			/*
			 * Rollback all current foreign transaction. Since we're rollbacking
			 * the transaction it's too late even if we raise an error here.
			 * So we log it as warning.
			 */
			if (!fdw_part->rollback_foreign_xact(state))
				ereport(WARNING,
						(errmsg("could not abort transaction on server \"%s\"",
								fdw_part->server->servername)));
		}

		/* If we left some FdwXact entries, ask the resolver process */
		if (left_fdwxacts > 0)
		{
			ereport(WARNING,
					(errmsg("might have left %u foreign transactions in in-doubt status",
							left_fdwxacts)));
			fdwxact_maybe_launch_resolver(true);
		}
	}

	ForgetAllFdwXactParticipants();
	FdwXactAtomicCommitReady = false;
}

/*
 * AtPrepare_FdwXacts
 *
 * If there are foreign servers involved in the transaction, this function
 * prepares transactions on those servers.
 *
 * Note that it can happen that the transaction aborts after we prepared part
 * of participants. In this case since we can change to abort we cannot forget
 * FdwXactAtomicCommitParticipants here. These are processed by the resolver process
 * during aborting, or at EOXact_FdwXacts.
 */
void
AtPrepare_FdwXacts(void)
{
	if (FdwXactAtomicCommitParticipants == NIL)
		return;

	/* Check for an invalid condition */
	if (!IsAtomicCommitRequested())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a distributed transaction when distributed_atomic_commit is \'disabled\'")));


	/*
	 * We cannot prepare if any foreign server of participants isn't capable
	 * of two-phase commit.
	 */
	if (FdwXactAtomicCommitRequired() &&
		(MyXactFlags & XACT_FLAGS_FDWNOPREPARE) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("can not prepare the transaction because some foreign servers involved in transaction can not prepare the transaction")));

	/* Prepare transactions on participating foreign servers. */
	FdwXactPrepareForeignTransactions();
}

/*
 * FdwXactResolveForeignTransaction
 *
 * Resolve the foreign transaction using the foreign data wrapper's transaction
 * handler routine. The foreign transaction can be a dangling transaction
 * that is not interested by nobody. If the fate of foreign transaction is
 * not determined yet, it'sdetermined according to the status of corresponding
 * local transaction.
 *
 * If the resolution is successful, remove the foreign transaction entry from
 * the shared memory and also remove the corresponding on-disk file.
 */
static bool
FdwXactResolveForeignTransaction(FdwXactState *state, FdwXact fdwxact,
								 int elevel)
{
	ForeignServer		*server;
	ForeignDataWrapper	*fdw;
	FdwRoutine			*fdw_routine;
	bool		is_commit;
	bool		ret;

	Assert(fdwxact);

	/*
	 * Determine whether we commit or abort this foreign transaction.
	 */
	if (fdwxact->status == FDW_XACT_COMMITTING_PREPARED)
		is_commit = true;
	else if (fdwxact->status == FDW_XACT_ABORTING_PREPARED)
		is_commit = false;

	/*
	 * If the local transaction is already committed, commit prepared
	 * foreign transaction.
	 */
	else if (TransactionIdDidCommit(fdwxact->local_xid))
	{
		fdwxact->status = FDW_XACT_COMMITTING_PREPARED;
		is_commit = true;
	}

	/*
	 * If the local transaction is already aborted, abort prepared
	 * foreign transactions.
	 */
	else if (TransactionIdDidAbort(fdwxact->local_xid))
	{
		fdwxact->status = FDW_XACT_ABORTING_PREPARED;
		is_commit = false;
	}

	/*
	 * The local transaction is not in progress but the foreign
	 * transaction is not prepared on the foreign server. This
	 * can happen when transaction failed after registered this
	 * entry but before actual preparing on the foreign server.
	 * So let's assume it aborted.
	 */
	else if (!TransactionIdIsInProgress(fdwxact->local_xid))
		is_commit = false;

	/*
	 * The Local transaction is in progress and foreign transaction
	 * state is neither committing or aborting. This should not
	 * happen because we cannot determine to do commit or abort for
	 * foreign transaction associated with the in-progress local
	 * transaction.
	 */
	else
		ereport(ERROR,
				(errmsg("cannot resolve the foreign transaction associated with in-progress transaction %u on server %u",
						fdwxact->local_xid, fdwxact->serverid)));

	server = GetForeignServer(fdwxact->serverid);
	fdw = GetForeignDataWrapper(server->fdwid);
	fdw_routine = GetFdwRoutine(fdw->fdwhandler);

	/* Resolve the foreign transaction */
	Assert(fdw_routine->ResolveForeignTransaction);

	ret = fdw_routine->ResolveForeignTransaction(state, is_commit);

	if (!ret)
	{
		ereport(elevel,
				(errmsg("could not %s a prepared foreign transaction on server \"%s\"",
						is_commit ? "commit" : "rollback", server->servername),
				 errdetail("local transaction id is %u, connected by user id %u",
						   fdwxact->local_xid, fdwxact->userid)));
	}

	/* Resolution was a success, remove the entry */
	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	if (fdwxact->ondisk)
		RemoveFdwXactFile(fdwxact->dbid, fdwxact->local_xid,
						  fdwxact->serverid, fdwxact->userid,
						  true);
	remove_fdw_xact(fdwxact);
	LWLockRelease(FdwXactLock);

	return ret;
}

static FdwXactState *
create_fdw_xact_state(void)
{
	FdwXactState *state;

	state = palloc(sizeof(FdwXactState));
	state->serverid = InvalidOid;
	state->userid = InvalidOid;
	state->umid = InvalidOid;
	state->fdwxact_id = NULL;
	state->fdw_state = NULL;

	return state;
}

/*
 * Return one FdwXact entry that matches to given arguments, otherwise
 * return NULL. Since this function search FdwXact entry by unique key
 * all arguments should be valid.
 */
static FdwXact
get_one_fdw_xact(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
				 bool need_lock)
{
	List	*fdw_xact_list;

	/* All search conditions must be valid values */
	Assert(TransactionIdIsValid(xid));
	Assert(OidIsValid(serverid));
	Assert(OidIsValid(userid));
	Assert(OidIsValid(dbid));

	fdw_xact_list = get_fdw_xacts(dbid, xid, serverid, userid, need_lock);

	/* Could not find entry */
	if (fdw_xact_list == NIL)
		return NULL;

	/* Must be one entry since we search it by the unique key */
	Assert(list_length(fdw_xact_list) == 1);

	return (FdwXact) linitial(fdw_xact_list);
}

/*
 * Return true if there is at least one prepared foreign transaction
 * which matches given arguments.
 */
bool
fdw_xact_exists(Oid dbid, TransactionId xid, Oid serverid, Oid userid)
{
	List	*fdw_xact_list;

	fdw_xact_list = get_fdw_xacts(dbid, xid, serverid, userid, true);

	return fdw_xact_list != NIL;
}

/*
 * Returns an array of all foreign prepared transactions for the user-level
 * function pg_prepared_fdw_xacts.
 *
 * WARNING -- we return even those transactions whose information is not
 * completely filled yet. The caller should filter them out if he doesn't want them.
 *
 * The returned array is palloc'd.
 */
static FdwXact
get_all_fdw_xacts(int *length)
{
	List		*all_fdw_xacts;
	ListCell	*lc;
	FdwXact		fdw_xacts;
	int			num_fdw_xacts = 0;

	Assert(length != NULL);

	/* Get all entries */
	all_fdw_xacts = get_fdw_xacts(InvalidOid, InvalidTransactionId,
								  InvalidOid, InvalidOid, true);

	if (all_fdw_xacts == NIL)
	{
		*length = 0;
		return NULL;
	}

	fdw_xacts = (FdwXact)
		palloc(sizeof(FdwXactData) * list_length(all_fdw_xacts));

	/* Convert list to array of FdwXact */
	foreach(lc, all_fdw_xacts)
	{
		FdwXact fx = (FdwXact) lfirst(lc);

		memcpy(fdw_xacts + num_fdw_xacts, fx,
			   sizeof(FdwXactData));
		num_fdw_xacts++;
	}

	*length = num_fdw_xacts;
	list_free(all_fdw_xacts);

	return fdw_xacts;
}

/*
 * Return a list of FdwXact matched to given arguments. Otherwise return
 * NIL.
 */
static List*
get_fdw_xacts(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
			  bool need_lock)
{
	int i;
	List	*fdw_xact_list = NIL;

	if (need_lock)
		LWLockAcquire(FdwXactLock, LW_SHARED);

	for (i = 0; i < FdwXactCtl->numFdwXacts; i++)
	{
		FdwXact	fdw_xact = FdwXactCtl->fdw_xacts[i];
		bool	matches = true;

		/* xid */
		if (xid != InvalidTransactionId && xid != fdw_xact->local_xid)
			matches = false;

		/* dbid */
		if (OidIsValid(dbid) && fdw_xact->dbid != dbid)
			matches = false;

		/* serverid */
		if (OidIsValid(serverid) && serverid != fdw_xact->serverid)
			matches = false;

		/* userid */
		if (OidIsValid(userid) && fdw_xact->userid != userid)
			matches = false;

		/* Append it if matched */
		if (matches)
			fdw_xact_list = lappend(fdw_xact_list, fdw_xact);
	}

	if (need_lock)
		LWLockRelease(FdwXactLock);

	return fdw_xact_list;
}

/*
 * fdw_xact_redo
 * Apply the redo log for a foreign transaction.
 */
void
fdw_xact_redo(XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_FDW_XACT_INSERT)
	{
		/*
		 * Add fdwxact entry and set start/end lsn of the WAL record
		 * in FdwXact entry.
		 */
		LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
		FdwXactRedoAdd(XLogRecGetData(record),
					   record->ReadRecPtr,
					   record->EndRecPtr);
		LWLockRelease(FdwXactLock);
	}
	else if (info == XLOG_FDW_XACT_REMOVE)
	{
		xl_fdw_xact_remove *record = (xl_fdw_xact_remove *) rec;

		/* Delete FdwXact entry and file if exists */
		LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
		FdwXactRedoRemove(record->dbid, record->xid, record->serverid,
						  record->userid, false);
		LWLockRelease(FdwXactLock);
	}
	else
		elog(ERROR, "invalid log type %d in foreign transction log record", info);

	return;
}

/*
 * Return a null-terminated foreign transaction identifier with in the form
 * of "fx_<random number>_<xid>_<serverid>_<userid> whose length is always
 * less than NAMEDATALEN.
 *
 * Returned string value is used to identify foreign transaction. The
 * identifier should not be same as any other concurrent prepared transaction
 * identifier.
 *
 * To make the foreign transactionid unique, we should ideally use something
 * like UUID, which gives unique ids with high probability, but that may be
 * expensive here and UUID extension which provides the function to generate
 * UUID is not part of the core code.
 */
static char *
generate_fdw_xact_identifier(TransactionId xid, Oid serverid, Oid userid)
{
	char*	fdw_xact_id;

	fdw_xact_id = (char *)palloc0(FDW_XACT_ID_MAX_LEN * sizeof(char));

	snprintf(fdw_xact_id, FDW_XACT_ID_MAX_LEN, "%s_%ld_%u_%d_%d",
			 "fx", Abs(random()), xid, serverid, userid);
	fdw_xact_id[strlen(fdw_xact_id)] = '\0';

	return fdw_xact_id;
}

/*
 * CheckPointFdwXact
 *
 * We must fsync the foreign transaction state file that is valid or generated
 * during redo and has a inserted LSN <= the checkpoint'S redo horizon.
 * The foreign transaction entries and hence the corresponding files are expected
 * to be very short-lived. By executing this function at the end, we might have
 * lesser files to fsync, thus reducing some I/O. This is similar to
 * CheckPointTwoPhase().
 *
 * In order to avoid disk I/O while holding a light weight lock, the function
 * first collects the files which need to be synced under FdwXactLock and then
 * syncs them after releasing the lock. This approach creates a race condition:
 * after releasing the lock, and before syncing a file, the corresponding
 * foreign transaction entry and hence the file might get removed. The function
 * checks whether that's true and ignores the error if so.
 */
void
CheckPointFdwXacts(XLogRecPtr redo_horizon)
{
	int			cnt;
	int			serialized_fdw_xacts = 0;

	/* Quick get-away, before taking lock */
	if (max_prepared_foreign_xacts <= 0)
		return;

	TRACE_POSTGRESQL_FDWXACT_CHECKPOINT_START();

	LWLockAcquire(FdwXactLock, LW_SHARED);

	/* Another quick, before we allocate memory */
	if (FdwXactCtl->numFdwXacts <= 0)
	{
		LWLockRelease(FdwXactLock);
		return;
	}

	/*
	 * We are expecting there to be zero FdwXact that need to be copied to
	 * disk, so we perform all I/O while holding FdwXactLock for simplicity.
	 * This presents any new foreign xacts from preparing while this occurs,
	 * which shouldn't be a problem since the presence fo long-lived prepared
	 * foreign xacts indicated the transaction manager isn't active.
	 *
	 * It's also possible to move I/O out of the lock, but on every error we
	 * should check whether somebody committed our transaction in different
	 * backend. Let's leave this optimisation for future, if somebody will
	 * spot that this place cause bottleneck.
	 *
	 * Note that it isn't possible for there to be a FdwXact with a
	 * insert_end_lsn set prior to the last checkpoint yet is marked
	 * invalid, because of the efforts with delayChkpt.
	 */
	for (cnt = 0; cnt < FdwXactCtl->numFdwXacts; cnt++)
	{
		FdwXact		fxact = FdwXactCtl->fdw_xacts[cnt];

		if ((fxact->valid || fxact->inredo) &&
			!fxact->ondisk &&
			fxact->insert_end_lsn <= redo_horizon)
		{
			char	   *buf;
			int			len;

			XlogReadFdwXactData(fxact->insert_start_lsn, &buf, &len);
			RecreateFdwXactFile(fxact->dbid, fxact->local_xid,
								fxact->serverid, fxact->userid,
								buf, len);
			fxact->ondisk = true;
			fxact->insert_start_lsn = InvalidXLogRecPtr;
			fxact->insert_end_lsn = InvalidXLogRecPtr;
			pfree(buf);
			serialized_fdw_xacts++;
		}
	}

	LWLockRelease(FdwXactLock);

	/*
	 * Flush unconditionally the parent directory to make any information
	 * durable on disk.  FdwXact files could have been removed and those
	 * removals need to be made persistent as well as any files newly created.
	 */
	fsync_fname(FDW_XACTS_DIR, true);

	TRACE_POSTGRESQL_FDWXACT_CHECKPOINT_DONE();

	if (log_checkpoints && serialized_fdw_xacts > 0)
		ereport(LOG,
			  (errmsg_plural("%u foreign transaction state file was written "
							 "for long-running prepared transactions",
							 "%u foreign transaction state files were written "
							 "for long-running prepared transactions",
							 serialized_fdw_xacts,
							 serialized_fdw_xacts)));
}

/*
 * Reads foreign transaction data from xlog. During checkpoint this data will
 * be moved to fdwxact files and ReadFdwXactFile should be used instead.
 *
 * Note clearly that this function accesses WAL during normal operation, similarly
 * to the way WALSender or Logical Decoding would do. It does not run during
 * crash recovery or standby processing.
 */
static void
XlogReadFdwXactData(XLogRecPtr lsn, char **buf, int *len)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;

	xlogreader = XLogReaderAllocate(wal_segment_size, &read_local_xlog_page, NULL);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
		   errdetail("Failed while allocating an XLog reading processor.")));

	record = XLogReadRecord(xlogreader, lsn, &errormsg);
	if (record == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
		errmsg("could not read foreign transaction state from xlog at %X/%X",
			   (uint32) (lsn >> 32),
			   (uint32) lsn)));

	if (XLogRecGetRmid(xlogreader) != RM_FDW_XACT_ID ||
		(XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK) != XLOG_FDW_XACT_INSERT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("expected foreign transaction state data is not present in xlog at %X/%X",
						(uint32) (lsn >> 32),
						(uint32) lsn)));

	if (len != NULL)
		*len = XLogRecGetDataLen(xlogreader);

	*buf = palloc(sizeof(char) * XLogRecGetDataLen(xlogreader));
	memcpy(*buf, XLogRecGetData(xlogreader), sizeof(char) * XLogRecGetDataLen(xlogreader));

	XLogReaderFree(xlogreader);
}

/*
 * Recreates a foreign transaction state file. This is used in WAL replay
 * and during checkpoint creation.
 *
 * Note: content and len don't include CRC.
 */
void
RecreateFdwXactFile(Oid dbid, TransactionId xid, Oid serverid,
					Oid userid, void *content, int len)
{
	char		path[MAXPGPATH];
	pg_crc32c	statefile_crc;
	int			fd;

	/* Recompute CRC */
	INIT_CRC32C(statefile_crc);
	COMP_CRC32C(statefile_crc, content, len);
	FIN_CRC32C(statefile_crc);

	FdwXactFilePath(path, dbid, xid, serverid, userid);

	fd = OpenTransientFile(path, O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
		errmsg("could not recreate foreign transaction state file \"%s\": %m",
			   path)));

	/* Write content and CRC */
	pgstat_report_wait_start(WAIT_EVENT_FDW_XACT_FILE_WRITE);
	if (write(fd, content, len) != len)
	{
		pgstat_report_wait_end();
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write foreign transcation state file: %m")));
	}
	if (write(fd, &statefile_crc, sizeof(pg_crc32c)) != sizeof(pg_crc32c))
	{
		pgstat_report_wait_end();
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write foreign transcation state file: %m")));
	}
	pgstat_report_wait_end();

	/*
	 * We must fsync the file because the end-of-replay checkpoint will not do
	 * so, there being no FDWXACT in shared memory yet to tell it to.
	 */
	pgstat_report_wait_start(WAIT_EVENT_FDW_XACT_FILE_SYNC);
	if (pg_fsync(fd) != 0)
	{
		pgstat_report_wait_end();
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not fsync foreign transaction state file: %m")));
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close foreign transaction file: %m")));
}

/*
 * ProcessFdwXactBuffer
 *
 * Given a transaction id, userid and serverid read it either from disk
 * or read it directly via shmem xlog record pointer using the provided
 * "insert_start_lsn".
 */
static char *
ProcessFdwXactBuffer(Oid dbid, TransactionId xid, Oid serverid,
					 Oid userid, XLogRecPtr insert_start_lsn, bool fromdisk)
{
	TransactionId	origNextXid = ShmemVariableCache->nextXid;
	char	*buf;

	Assert(LWLockHeldByMeInMode(FdwXactLock, LW_EXCLUSIVE));

	if (!fromdisk)
		Assert(insert_start_lsn != InvalidXLogRecPtr);

	if (TransactionIdFollowsOrEquals(xid, origNextXid))
	{
		if (fromdisk)
		{
			ereport(WARNING,
					(errmsg("removing future fdwxact state file for xid %u, server %u and user %u",
							xid, serverid, userid)));
			RemoveFdwXactFile(dbid, xid, serverid, userid, true);
		}
		else
		{
			ereport(WARNING,
					(errmsg("removing future fdwxact state from memory for xid %u, server %u and user %u",
							xid, serverid, userid)));
			FdwXactRedoRemove(dbid, xid, serverid, userid, true);
		}
		return NULL;
	}

	if (fromdisk)
	{
		buf = ReadFdwXactFile(dbid, xid, serverid, userid, true);
		if (buf == NULL)
		{
			ereport(WARNING,
					(errmsg("removing corrupt fdwxact state file for xid %u, server %u and user %u",
							xid, serverid, userid)));
			RemoveFdwXactFile(dbid, xid, serverid, userid, true);
			return NULL;
		}
	}
	else
	{
		/* Read xlog data */
		XlogReadFdwXactData(insert_start_lsn, &buf, NULL);
	}

	return buf;
}

/*
 * Read and validate the foreign transaction state file.
 *
 * If it looks OK (has a valid magic number and CRC), return thecontents in
 * a structure allocated in-memory. Otherwise return NULL. The structure can
 * be later freed by the caller.
 */
static char *
ReadFdwXactFile(Oid dbid, TransactionId xid, Oid serverid, Oid userid,
				bool give_warnings)
{
	char		path[MAXPGPATH];
	int			fd;
	FdwXactOnDiskData *fxact_file_data;
	struct stat stat;
	uint32		crc_offset;
	pg_crc32c	calc_crc;
	pg_crc32c	file_crc;
	char	   *buf;

	FdwXactFilePath(path, dbid, xid, serverid, userid);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
			   errmsg("could not open FDW transaction state file \"%s\": %m",
					  path)));

	/*
	 * Check file length.  We can determine a lower bound pretty easily. We
	 * set an upper bound to avoid palloc() failure on a corrupt file, though
	 * we can't guarantee that we won't get an out of memory error anyway,
	 * even on a valid file.
	 */
	if (fstat(fd, &stat))
	{
		CloseTransientFile(fd);
		if (give_warnings)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not stat FDW transaction state file \"%s\": %m",
							path)));
		return NULL;
	}

	if (stat.st_size < (offsetof(FdwXactOnDiskData, fdw_xact_id) +
						sizeof(pg_crc32c)) ||
		stat.st_size > MaxAllocSize)
	{
		CloseTransientFile(fd);
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("too large FDW transaction state file \"%s\": %m",
						path)));
		return NULL;
	}

	crc_offset = stat.st_size - sizeof(pg_crc32c);
	if (crc_offset != MAXALIGN(crc_offset))
	{
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * Ok, slurp in the file.
	 */
	buf = (char *) palloc(stat.st_size);
	fxact_file_data = (FdwXactOnDiskData *) buf;

	/* Slurp the file */
	pgstat_report_wait_start(WAIT_EVENT_FDW_XACT_FILE_READ);
	if (read(fd, buf, stat.st_size) != stat.st_size)
	{
		pgstat_report_wait_end();
		CloseTransientFile(fd);
		if (give_warnings)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not read FDW transaction state file \"%s\": %m",
					  path)));
		return NULL;
	}

	pgstat_report_wait_end();
	CloseTransientFile(fd);

	/*
	 * Check the CRC.
	 */
	INIT_CRC32C(calc_crc);
	COMP_CRC32C(calc_crc, buf, crc_offset);
	FIN_CRC32C(calc_crc);

	file_crc = *((pg_crc32c *) (buf + crc_offset));

	if (!EQ_CRC32C(calc_crc, file_crc))
	{
		pfree(buf);
		return NULL;
	}

	/* Check if the contents is an expected data */
	fxact_file_data = (FdwXactOnDiskData *) buf;
	if (fxact_file_data->dbid  != dbid ||
		fxact_file_data->serverid != serverid ||
		fxact_file_data->userid != userid ||
		fxact_file_data->local_xid != xid)
	{
		ereport(WARNING,
			(errmsg("invalid foreign transaction state file \"%s\"",
					path)));
		CloseTransientFile(fd);
		pfree(buf);
		return NULL;
	}

	return buf;
}

/*
 * PrescanFdwXacts
 *
 * Scan the all foreign transactions directory for oldest active transaction.
 * This is run during database startup, after we completed reading WAL.
 * ShmemVariableCache->nextXid has been set to one more than the highest XID
 * for which evidence exists in WAL.
 */
TransactionId
PrescanFdwXacts(TransactionId oldestActiveXid)
{
	TransactionId nextXid = ShmemVariableCache->nextXid;
	DIR		   *cldir;
	struct dirent *clde;

	cldir = AllocateDir(FDW_XACTS_DIR);
	while ((clde = ReadDir(cldir, FDW_XACTS_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == FDW_XACT_FILE_NAME_LEN &&
		 strspn(clde->d_name, "0123456789ABCDEF_") == FDW_XACT_FILE_NAME_LEN)
		{
			Oid			dbid;
			Oid			serverid;
			Oid			userid;
			TransactionId local_xid;

			sscanf(clde->d_name, "%08x_%08x_%08x_%08x",
				   &dbid, &local_xid, &serverid, &userid);

			/*
			 * Remove a foreign prepared transaction file corresponding to an
			 * XID, which is too new.
			 */
			if (TransactionIdFollowsOrEquals(local_xid, nextXid))
			{
				ereport(WARNING,
						(errmsg("removing future foreign prepared transaction file \"%s\"",
								clde->d_name)));
				RemoveFdwXactFile(dbid, local_xid, serverid, userid, true);
				continue;
			}

			if (TransactionIdPrecedesOrEquals(local_xid, oldestActiveXid))
				oldestActiveXid = local_xid;
		}
	}

	FreeDir(cldir);
	return oldestActiveXid;
}

/*
 * restoreFdwXactData
 *
 * Scan pg_fdw_xact and fill FdwXact depending on the on-disk data.
 * This is called once at the beginning of recovery, saving any extra
 * lookups in the future.  FdwXact files that are newer than the
 * minimum XID horizon are discarded on the way.
 */
void
restoreFdwXactData(void)
{
	DIR		   *cldir;
	struct dirent *clde;

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	cldir = AllocateDir(FDW_XACTS_DIR);
	while ((clde = ReadDir(cldir, FDW_XACTS_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == FDW_XACT_FILE_NAME_LEN &&
			strspn(clde->d_name, "0123456789ABCDEF_") == FDW_XACT_FILE_NAME_LEN)
		{
			TransactionId local_xid;
			Oid			dbid;
			Oid			serverid;
			Oid			userid;
			char		*buf;

			sscanf(clde->d_name, "%08x_%08x_%08x_%08x",
				   &dbid, &local_xid, &serverid, &userid);

			/* Read fdwxact data from disk */
			buf = ProcessFdwXactBuffer(dbid, local_xid, serverid, userid,
									   InvalidXLogRecPtr, true);

			if (buf == NULL)
				continue;

			/* Add this entry into the table of foreign transactions */
			FdwXactRedoAdd(buf, InvalidXLogRecPtr, InvalidXLogRecPtr);
		}
	}

	LWLockRelease(FdwXactLock);
	FreeDir(cldir);
}

/*
 * Remove the foreign transaction file for given entry.
 *
 * If giveWarning is false, do not complain about file-not-present;
 * this is an expected case during WAL replay.
 */
static void
RemoveFdwXactFile(Oid dbid, TransactionId xid, Oid serverid, Oid userid, bool giveWarning)
{
	char		path[MAXPGPATH];

	FdwXactFilePath(path, dbid, xid, serverid, userid);
	if (unlink(path) < 0 && (errno != ENOENT || giveWarning))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove foreign transaction state file \"%s\": %m",
						path)));
}

/*
 * FdwXactRedoAdd
 *
 * Store pointer to the start/end of the WAL record along with the xid in
 * a fdwxact entry in shared memory FdwXactData structure.
 */
static void
FdwXactRedoAdd(char *buf, XLogRecPtr start_lsn, XLogRecPtr end_lsn)
{
	FdwXactOnDiskData *fxact_data = (FdwXactOnDiskData *) buf;
	FdwXact fxact;

	Assert(LWLockHeldByMeInMode(FdwXactLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	/*
	 * Add this entry into the table of foreign transactions. The
	 * status of the transaction is set as preparing, since we do not
	 * know the exact status right now. Resolver will set it later
	 * based on the status of local transaction which prepared this
	 * foreign transaction.
	 */
	fxact = insert_fdw_xact(fxact_data->dbid, fxact_data->local_xid,
							fxact_data->serverid, fxact_data->userid,
							fxact_data->umid, fxact_data->fdw_xact_id);

	/*
	 * Set status as preparing, since we do not know the xact status
	 * right now. Resolver will set it later based on the status of
	 * local transaction that prepared this fdwxact entry.
	 */
	fxact->status = FDW_XACT_PREPARING;
	fxact->insert_start_lsn = start_lsn;
	fxact->insert_end_lsn = end_lsn;
	fxact->inredo = true;	/* added in redo */
	fxact->valid = false;
	fxact->ondisk = XLogRecPtrIsInvalid(start_lsn);
}

/*
 * FdwXactRedoRemove
 *
 * Remove the corresponding fdw_xact entry from FdwXactCtl.
 * Also remove fdw_xact file if a foreign transaction was saved
 * via an earlier checkpoint.
 */
void
FdwXactRedoRemove(Oid dbid, TransactionId xid, Oid serverid,
				  Oid userid, bool givewarning)
{
	FdwXact	fdwxact;

	Assert(LWLockHeldByMeInMode(FdwXactLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	fdwxact = get_one_fdw_xact(dbid, xid, serverid, userid,
							   false);

	if (fdwxact == NULL)
		return;

	/* Clean up entry and any files we may have left */
	if (fdwxact->ondisk)
		RemoveFdwXactFile(fdwxact->dbid, fdwxact->local_xid,
						  fdwxact->serverid, fdwxact->userid,
						  givewarning);
	remove_fdw_xact(fdwxact);
}

/*
 * Scan the shared memory entries of FdwXact and valid them.
 *
 * This is run at the end of recovery, but before we allow backends to write
 * WAL.
 */
void
RecoverFdwXacts(void)
{
	int i;

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);
	for (i = 0; i < FdwXactCtl->numFdwXacts; i++)
	{
		FdwXact fdwxact = FdwXactCtl->fdw_xacts[i];
		char	*buf;

		buf = ProcessFdwXactBuffer(fdwxact->dbid, fdwxact->local_xid,
								   fdwxact->serverid, fdwxact->userid,
								   fdwxact->insert_start_lsn, fdwxact->ondisk);

		if (buf == NULL)
			continue;

		ereport(LOG,
				(errmsg("recovering foreign transaction %u for server %u and user %u from shared memory",
						fdwxact->local_xid, fdwxact->serverid, fdwxact->userid)));

		fdwxact->inredo = false;
		fdwxact->valid = true;
	}
	LWLockRelease(FdwXactLock);
}

bool
check_distributed_atomic_commit(int *newval, void **extra, GucSource source)
{
	DistributedAtomicCommitLevel newDistributedAtomicCommitLevel = *newval;

		/* Parameter check */
	if (newDistributedAtomicCommitLevel > DISTRIBUTED_ATOMIC_COMMIT_DISABLED &&
		(max_prepared_foreign_xacts == 0 || max_foreign_xact_resolvers == 0))
	{
		GUC_check_errdetail("Cannot enable \"distributed_atomic_commit\" when "
							"\"max_prepared_foreign_transactions\" or \"max_foreign_transaction_resolvers\""
							"is zero value");
		return false;
	}

	return true;
}

/* Built in functions */
/*
 * Structure to hold and iterate over the foreign transactions to be displayed
 * by the built-in functions.
 */
typedef struct
{
	FdwXact		fdw_xacts;
	int			num_xacts;
	int			cur_xact;
}	WorkingStatus;

Datum
pg_prepared_fdw_xacts(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	WorkingStatus *status;
	char	   *xact_status;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		int			num_fdw_xacts = 0;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_fdw_xacts view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(6, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "dbid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "transaction",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "serverid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "userid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "status",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "identifier",
						   TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect status information that we will format and send out as a
		 * result set.
		 */
		status = (WorkingStatus *) palloc(sizeof(WorkingStatus));
		funcctx->user_fctx = (void *) status;

		status->fdw_xacts = get_all_fdw_xacts(&num_fdw_xacts);
		status->num_xacts = num_fdw_xacts;
		status->cur_xact = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	status = funcctx->user_fctx;

	while (status->cur_xact < status->num_xacts)
	{
		FdwXact		fdw_xact = &status->fdw_xacts[status->cur_xact++];
		Datum		values[6];
		bool		nulls[6];
		HeapTuple	tuple;
		Datum		result;

		if (!fdw_xact->valid)
			continue;

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(fdw_xact->dbid);
		values[1] = TransactionIdGetDatum(fdw_xact->local_xid);
		values[2] = ObjectIdGetDatum(fdw_xact->serverid);
		values[3] = ObjectIdGetDatum(fdw_xact->userid);
		switch (fdw_xact->status)
		{
			case FDW_XACT_PREPARING:
				xact_status = "prepared";
				break;
			case FDW_XACT_COMMITTING_PREPARED:
				xact_status = "committing";
				break;
			case FDW_XACT_ABORTING_PREPARED:
				xact_status = "aborting";
				break;
			default:
				xact_status = "unknown";
				break;
		}
		values[4] = CStringGetTextDatum(xact_status);
		/* should this be really interpreted by FDW */
		values[5] = PointerGetDatum(cstring_to_text_with_len(fdw_xact->fdw_xact_id,
															 strlen(fdw_xact->fdw_xact_id)));

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Built-in function to resolve a prepared foreign transaction manually.
 */
Datum
pg_resolve_fdw_xact(PG_FUNCTION_ARGS)
{
	TransactionId	xid = DatumGetTransactionId(PG_GETARG_DATUM(0));
	Oid				serverid = PG_GETARG_OID(1);
	Oid				userid = PG_GETARG_OID(2);
	FdwXactState *state;
	UserMapping		*usermapping;
	FdwXact			fdwxact;
	bool			ret;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to resolve foreign transactions"))));

	fdwxact = get_one_fdw_xact(MyDatabaseId, xid, serverid, userid, true);

	if (fdwxact == NULL)
		PG_RETURN_BOOL(false);

	usermapping = GetUserMapping(userid, serverid);

	state = create_fdw_xact_state();
	state->serverid = serverid;
	state->userid = userid;
	state->umid = usermapping->umid;

	ret = FdwXactResolveForeignTransaction(state, fdwxact, LOG);

	PG_RETURN_BOOL(ret);
}

/*
 * Built-in function to remove a prepared foreign transaction entry without
 * resolution. The function gives a way to forget about such prepared
 * transaction in case: the foreign server where it is prepared is no longer
 * available, the user which prepared this transaction needs to be dropped.
 */
Datum
pg_remove_fdw_xact(PG_FUNCTION_ARGS)
{
	TransactionId	xid = DatumGetTransactionId(PG_GETARG_DATUM(0));
	Oid				serverid = PG_GETARG_OID(1);
	Oid				userid = PG_GETARG_OID(2);
	FdwXact			fdwxact;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to remove foreign transactions"))));

	LWLockAcquire(FdwXactLock, LW_EXCLUSIVE);

	fdwxact = get_one_fdw_xact(MyDatabaseId, xid, serverid, userid, false);
	if (fdwxact == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 (errmsg("could not find foreign transaction entry"))));

	remove_fdw_xact(fdwxact);

	LWLockRelease(FdwXactLock);

	PG_RETURN_VOID();
}
