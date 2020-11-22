/*-------------------------------------------------------------------------
 *
 * pg_statement_rollback.c
 *
 *    Add support to Oracle/DB2 style rollback at statement level in PostgreSQL.
 *    This feature is entirely driven at server side, the client don't have to
 *    execute a ROLLBACK TO automatic savepoint in case of error. In case of
 *    a statement error the transaction will be rolled back to the state of the
 *    last successful statement.
 *
 * Authors: Julien Rouhaud, Dave Sharpe, Gilles Darold 
 * Licence: PostgreSQL
 * Copyright (c) 2020 LzLabs, GmbH
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <access/xact.h>
#include <commands/portalcmds.h>
#include <executor/executor.h>
#include <nodes/pg_list.h>
#include <optimizer/planner.h>
#include <tcop/pquery.h>
#include <tcop/utility.h>
#include <utils/elog.h>
#include <utils/guc.h>

/* Define ProcessUtility hook proto/parameters following the PostgreSQL version */
#if PG_VERSION_NUM >= 130000
#define SLR_PROCESSUTILITY_PROTO PlannedStmt *pstmt, const char *queryString, \
					ProcessUtilityContext context, ParamListInfo params, \
					QueryEnvironment *queryEnv, DestReceiver *dest, \
					QueryCompletion *qc
#define SLR_PROCESSUTILITY_ARGS pstmt, queryString, context, params, queryEnv, dest, qc
#define SLR_PLANNERHOOK_PROTO Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams
#define SLR_PLANNERHOOK_ARGS parse, query_string, cursorOptions, boundParams
#else
#define SLR_PLANNERHOOK_PROTO Query *parse, int cursorOptions, ParamListInfo boundParams
#define SLR_PLANNERHOOK_ARGS parse, cursorOptions, boundParams
#if PG_VERSION_NUM >= 100000
#define SLR_PROCESSUTILITY_PROTO PlannedStmt *pstmt, const char *queryString, \
					ProcessUtilityContext context, ParamListInfo params, \
					QueryEnvironment *queryEnv, DestReceiver *dest, \
					char *completionTag
#define SLR_PROCESSUTILITY_ARGS pstmt, queryString, context, params, queryEnv, dest, completionTag
#elif PG_VERSION_NUM >= 90300
#define SLR_PROCESSUTILITY_PROTO Node *parsetree, const char *queryString, \
                                        ProcessUtilityContext context, ParamListInfo params, \
					DestReceiver *dest, char *completionTag
#define SLR_PROCESSUTILITY_ARGS parsetree, queryString, context, params, dest, completionTag
#else
#define SLR_PROCESSUTILITY_PROTO Node *parsetree, const char *queryString, \
                                        ParamListInfo params, bool isTopLevel, \
					DestReceiver *dest, char *completionTag
#define SLR_PROCESSUTILITY_ARGS parsetree, queryString, params, isTopLevel, dest, completionTag
#endif
#endif

PG_MODULE_MAGIC;

/* Variables to saved hook values in case of unload */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static XactCommandStart_hook_type prev_XactCommandStart = NULL;
static XactCommandFinish_hook_type prev_XactCommandFinish = NULL;
static AbortCurrentTransaction_hook_type prev_AbortCurrentTransaction = NULL;

/* Functions used with hooks */
static void slr_start_xact_command(void);
static void slr_finish_xact_command(void);
static void slr_abort_current_transaction(void);
static void slr_ExecutorEnd(QueryDesc *queryDesc);
static void slr_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
#if PG_VERSION_NUM >= 90600
                 uint64 count
#else
                 long count
#endif
#if PG_VERSION_NUM >= 100000
                 ,bool execute_once
#endif
	);
static void slr_ExecutorFinish(QueryDesc *queryDesc);
static void slr_ProcessUtility(SLR_PROCESSUTILITY_PROTO);
static PlannedStmt* slr_planner(SLR_PLANNERHOOK_PROTO);

/* Functions */
void	_PG_init(void);
void	_PG_fini(void);
bool slr_is_write_query(QueryDesc *queryDesc);

/* Global variables for automatic savepoint */
bool    slr_enabled        = true;
bool    slr_xact_opened    = false;
static int      slr_nest_executor_level = 0;
static bool     slr_planner_done = false;
static int      slr_nest_planner_level = 0;

bool exec_savepoint = false;
bool exec_release = false;
bool exec_rollbackto = false;
bool has_been_rolledback_and_released = false;

/*
 * Module load callback
 */
void
_PG_init(void)
{

	/*
	 * Install hooks.
	 */
	prev_planner_hook = planner_hook;
	planner_hook = slr_planner;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = slr_ExecutorRun;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = slr_ExecutorEnd;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = slr_ExecutorFinish;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = slr_ProcessUtility;

        prev_XactCommandStart = start_xact_command_hook;
	start_xact_command_hook = slr_start_xact_command;

        prev_XactCommandFinish = finish_xact_command_hook;
	finish_xact_command_hook = slr_finish_xact_command;

        prev_AbortCurrentTransaction = abort_current_transaction_hook;
	abort_current_transaction_hook = slr_abort_current_transaction;

	/*
	 * Automatic savepoint
	 *
	 */
	DefineCustomBoolVariable(
		"pg_statement_rollback.enabled",
		"Enable automatic savepoint",
		NULL,
		&slr_enabled,
		true,
		PGC_USERSET,    /* Any user can set it */
		0,
		NULL,           /* No check hook */
		NULL,           /* No assign hook */
		NULL            /* No show hook */
	);
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	planner_hook = prev_planner_hook;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
	ProcessUtility_hook = prev_ProcessUtility;
        start_xact_command_hook = prev_XactCommandStart;
        finish_xact_command_hook = prev_XactCommandFinish;
        abort_current_transaction_hook = prev_AbortCurrentTransaction;
}

/* Keep track that the planner stage is fully terminated */
static PlannedStmt*
slr_planner(SLR_PLANNERHOOK_PROTO)
{
	PlannedStmt *stmt;

	/*
	 * For planners at executor level 0, remember that
	 * we didn't finish the planner stage yet
	 */
	if (slr_nest_executor_level == 0 && slr_nest_planner_level == 0)
		slr_planner_done = false;

	slr_nest_planner_level++;
	elog(DEBUG1, "RSL: increase nest planner level (slr_nest_executor_level %d, slr_nest_planner_level %d, slr_planner_done %d).",
			slr_nest_executor_level, slr_nest_planner_level, slr_planner_done);

	if (prev_planner_hook)
		stmt = prev_planner_hook(SLR_PLANNERHOOK_ARGS);
	else
		stmt = standard_planner(SLR_PLANNERHOOK_ARGS);

	slr_nest_planner_level--;

	/* Remember that the planner stage is now done */
	if (slr_nest_executor_level == 0 && slr_nest_planner_level == 0)
	{
		elog(DEBUG1, "RSL: planner_hook mark planner stage as done.");
		slr_planner_done = true;
	}

	return stmt;
}

static void
slr_ProcessUtility(SLR_PROCESSUTILITY_PROTO)
{
#if PG_VERSION_NUM >= 100000
	Node *parsetree = pstmt->utilityStmt;
#endif

	/* SPI calls are internal */
	if (dest->mydest == DestSPI)
	{
		/* do nothing */
	}
	else if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		/* detect if we are in a transaction or not */
		switch (stmt->kind)
		{
			case TRANS_STMT_PREPARE:
				/* Savepoints do not work with 2PC, so disable automatic
				 * savepoint.  Since a PREPARE TRANSACTION will actually
				 * detach the transaction from the current session, the
				 * transaction is not opened anymore anyway. */
				elog(DEBUG1, "RSL: mark the transaction as closed with PREPARE.");
				slr_xact_opened = false;
				break;
			case TRANS_STMT_BEGIN:
			case TRANS_STMT_START:
				/*
				 * we'll need to add a savepoint after the utility execution,
				 * but only if this is a top level statement, and we're not
				 * already in transaction. This is required to be able to
				 * execute a Release and Savepoint before next statement,
				 * otherwise the release will fail.
				 */
				if (slr_enabled && slr_nest_executor_level == 0 && !slr_xact_opened)
					exec_savepoint = true;

				/* mark the transaction as opened in all cases */
				elog(LOG, "RSL: mark the transaction as opened with BEGIN/START, exec_savepoint=%d.", exec_savepoint);
				slr_xact_opened = true;
				break;
			case TRANS_STMT_COMMIT:
			case TRANS_STMT_COMMIT_PREPARED:
			case TRANS_STMT_ROLLBACK_PREPARED:
			case TRANS_STMT_ROLLBACK:
				elog(DEBUG1, "RSL: mark the transaction as closed with ROLLBACK.");
				slr_xact_opened = false;
				break;
			case TRANS_STMT_SAVEPOINT:
				/*
				 * If client send a SAVEPOINT order we must add our own
				 * automatic savepoint to not lost client's in case of
				 * rollback.
				 */
				if (slr_enabled && stmt->savepoint_name != NULL)
					exec_savepoint = true;
				break;
			case TRANS_STMT_RELEASE:
				/* do nothing on RELEASE SAVEPOINT call */
				break;
			case TRANS_STMT_ROLLBACK_TO:
				/* explicit SAVEPOINT handling, do nothing */
				break;
			default:
				elog(ERROR, "RSL: Unexpected transaction kind %d.", stmt->kind);
				break;
		}
	}
	else if (IsA(parsetree, FetchStmt))
	{
		/* do nothing if it's a FETCH */
	}
	else if (slr_enabled && ( IsA(parsetree, DeclareCursorStmt) ||
				IsA(parsetree, PlannedStmt) ) )
	{
		/* The automatic savepoint is required for DECLARE not PLANNED */
		if (slr_enabled && IsTransactionBlock() && IsA(parsetree, DeclareCursorStmt))
		{
			exec_release = true;
			exec_savepoint = true;
		}

	}
	else if (!IsA(parsetree, ClosePortalStmt))
	{
		/*
		 * release automatic savepoint if any, and create a new one.
		 * We don't check for the planner stage here, since utilities
		 * go straight from parsing to executor without a planner stage.
		 */
		if (slr_enabled && slr_nest_executor_level == 0 && IsTransactionBlock())
		{
			exec_release = true;
			exec_savepoint = true;
		}
	}

	/* Continue the execution of the query */
	slr_nest_executor_level++;

	elog(DEBUG1, "SLR DEBUG: restore ProcessUtility.");
	/* Excecute the utility command, we are not concerned */
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(SLR_PROCESSUTILITY_ARGS);
		else
			standard_ProcessUtility(SLR_PROCESSUTILITY_ARGS);
		slr_nest_executor_level--;
	}
	PG_CATCH();
	{
		slr_nest_executor_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();

}

/*
 * XactCommandStart hook:
 * Create a subtransaction, release the previous one or rollback
 * and release the subtransaction in case of error.
 */
static void
slr_start_xact_command()
{
	if (slr_enabled && slr_nest_executor_level == 0)
	{
		if (exec_rollbackto)
		{
			elog(DEBUG1, "Automatic Rollback And Release");
			RollbackAndReleaseCurrentSubTransaction();
			exec_rollbackto = false;
			exec_release = false;
			exec_savepoint = true;
			has_been_rolledback_and_released = true;

		}
		if (exec_release)
		{
			elog(DEBUG1, "Automatic Realease");
			ReleaseCurrentSubTransaction();
			exec_release = false;
		}

		if (exec_savepoint)
		{
			elog(DEBUG1, "Automatic Savepoint");
			BeginInternalSubTransaction(NULL);
			exec_savepoint = false;
		}
	}

	if (prev_XactCommandStart)
		prev_XactCommandStart();
}

/*
 * XactCommandFinish hook:
 * Unused for the moment.
 */
static void
slr_finish_xact_command()
{
	/* do nothing */
	if (prev_XactCommandFinish)
		prev_XactCommandFinish();

}

/*
 * AbortCurrentTransaction hook:
 * Inform slr_start_xact_command() that a ROLLBACK TO savepoint must
 * be issued before executing the next statement.
 */
static void
slr_abort_current_transaction()
{
	if (slr_enabled)
	{
		elog(DEBUG1, "slr_abort_current_transaction(): enable exec_rollbackto");
		exec_rollbackto = true;
	}

	
	if (prev_AbortCurrentTransaction)
		prev_AbortCurrentTransaction();
}

/*
 * ExecutorRun hook: we track nesting depth.
 */
static void
slr_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
#if PG_VERSION_NUM >= 90600
                 uint64 count
#else
                 long count
#endif
#if PG_VERSION_NUM >= 100000
                 , bool execute_once
#endif
	)
{
	elog(DEBUG1, "RSL: ExecutorRun increasing slr_nest_executor_level.");
	slr_nest_executor_level++;

	PG_TRY();
	{
		if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 100000
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
#else
			prev_ExecutorRun(queryDesc, direction, count);
#endif
		else
#if PG_VERSION_NUM >= 100000
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#else
			standard_ExecutorRun(queryDesc, direction, count);
#endif
		elog(DEBUG1, "RSL: ExecutorRun decreasing slr_nest_executor_level.");
		slr_nest_executor_level--;
	}
	PG_CATCH();
	{
		elog(DEBUG1, "RSL: ExecutorRun decreasing slr_nest_executor_level.");
		slr_nest_executor_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
slr_ExecutorFinish(QueryDesc *queryDesc)
{

	elog(DEBUG1, "RSL: ExecutorFinish increasing slr_nest_executor_level.");
	slr_nest_executor_level++;

	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		slr_nest_executor_level--;
		elog(DEBUG1, "RSL: ExecutorFinish decreasing slr_nest_executor_level.");
	}
	PG_CATCH();
	{
		slr_nest_executor_level--;
		elog(DEBUG1, "RSL: ExecutorFinish decreasing slr_nest_executor_level.");
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/* ExecutorEnd hook: 
 * Inform slr_start_xact_command() that a RELEASE and/or SAVEPOINT must
 * be issued before executing the next statement.
 *
 * Be careful though, the planner can spawn multiple level of executors,
 * and we can't interfere with savepoints at that time.  We detect that we
 * passed the planner stage with the planner hook.
 */
static void
slr_ExecutorEnd(QueryDesc *queryDesc)
{
	/*
	 * Only handle automatic savepoints when we are in a transaction
	 */
	if (slr_enabled && slr_nest_executor_level == 0 &&
			slr_planner_done && IsTransactionBlock())
	{
		elog(DEBUG1, "RSL: ExecutorEnd (slr_nest_executor_level %d, slr_planner_done %d, operation %d).",
				slr_nest_executor_level, slr_planner_done, queryDesc->operation);

		/*
		 * If we are just after a RollbackAndRelease() ask for
		 * a SAVEPOINT to be able to handle a new error.
		 */
		if (has_been_rolledback_and_released)
		{
			exec_savepoint = true;
			has_been_rolledback_and_released = false;
		}
		/* Ask for a RELEASE+SAVEPOINT after a write statement */
		else if (slr_is_write_query(queryDesc))
		{
			exec_savepoint = true;
			exec_release = true;
		}
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Check that the query does not imply any writes to any tables.
 */
bool
slr_is_write_query(QueryDesc *queryDesc)
{
	ListCell   *l;

	/*
	 * Fail if write permissions are requested in parallel mode for table
	 * (temp or non-temp), otherwise fail for any non-temp table.
	 */
	foreach(l, queryDesc->plannedstmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind != RTE_RELATION)
			continue;

		if ((rte->requiredPerms & (~ACL_SELECT)) == 0)
			continue;

		return true;
	}

	return false;
}
