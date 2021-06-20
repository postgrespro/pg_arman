/*-------------------------------------------------------------------------
 *
 * restore.c: restore DB cluster and archived WAL.
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2021, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include "access/timeline.h"

#include <sys/stat.h>
#include <unistd.h>

#include "utils/thread.h"

typedef struct
{
	parray	   *pgdata_files;
	parray	   *dest_files;
	pgBackup   *dest_backup;
	parray	   *dest_external_dirs;
	parray	   *parent_chain;
	parray	   *dbOid_exclude_list;
	bool		skip_external_dirs;
	const char *to_root;
	size_t		restored_bytes;
	bool        use_bitmap;
	IncrRestoreMode        incremental_mode;
	XLogRecPtr  shift_lsn;    /* used only in LSN incremental_mode */

	/*
	 * Return value from the thread.
	 * 0 means there is no error, 1 - there is an error.
	 */
	int			ret;
} restore_files_arg;


static void
print_recovery_settings(InstanceState *instanceState, FILE *fp, pgBackup *backup,
							   pgRestoreParams *params, pgRecoveryTarget *rt);
static void
print_standby_settings_common(FILE *fp, pgBackup *backup, pgRestoreParams *params);

#if PG_VERSION_NUM >= 120000
static void
update_recovery_options(InstanceState *instanceState, pgBackup *backup,
						pgRestoreParams *params, pgRecoveryTarget *rt);
#else
static void
update_recovery_options_before_v12(InstanceState *instanceState, pgBackup *backup,
								   pgRestoreParams *params, pgRecoveryTarget *rt);
#endif

static void create_recovery_conf(InstanceState *instanceState, time_t backup_id,
								 pgRecoveryTarget *rt,
								 pgBackup *backup,
								 pgRestoreParams *params);
static void *restore_files(void *arg);
static size_t restore_file(pgFile *dest_file, const char *to_fullpath, bool already_exists, char *out_buf,
	pgBackup *dest_backup, parray *parent_chain, bool use_bitmap, IncrRestoreMode incremental_mode, XLogRecPtr shift_lsn);
static void set_orphan_status(parray *backups, pgBackup *parent_backup);

static void restore_chain(pgBackup *dest_backup, parray *parent_chain,
						  parray *dbOid_exclude_list, pgRestoreParams *params,
						  const char *pgdata_path, bool no_sync, bool cleanup_pgdata,
						  bool backup_has_tblspc);
static DestDirIncrCompatibility check_incremental_compatibility(const char *pgdata, uint64 system_identifier,
																IncrRestoreMode incremental_mode);

/*
 * Iterate over backup list to find all ancestors of the broken parent_backup
 * and update their status to BACKUP_STATUS_ORPHAN
 */
static void
set_orphan_status(parray *backups, pgBackup *parent_backup)
{
	/* chain is intact, but at least one parent is invalid */
	char	*parent_backup_id;
	int		j;

	/* parent_backup_id is a human-readable backup ID  */
	parent_backup_id = base36enc_dup(parent_backup->start_time);

	for (j = 0; j < parray_num(backups); j++)
	{

		pgBackup *backup = (pgBackup *) parray_get(backups, j);

		if (is_parent(parent_backup->start_time, backup, false))
		{
			if (backup->status == BACKUP_STATUS_OK ||
				backup->status == BACKUP_STATUS_DONE)
			{
				write_backup_status(backup, BACKUP_STATUS_ORPHAN, true);

				elog(WARNING,
					"Backup %s is orphaned because his parent %s has status: %s",
					base36enc(backup->start_time),
					parent_backup_id,
					status2str(parent_backup->status));
			}
			else
			{
				elog(WARNING, "Backup %s has parent %s with status: %s",
						base36enc(backup->start_time), parent_backup_id,
						status2str(parent_backup->status));
			}
		}
	}
	pg_free(parent_backup_id);
}

/*
 * Entry point of pg_probackup RESTORE and VALIDATE subcommands.
 */
int
do_restore_or_validate(InstanceState *instanceState, time_t target_backup_id, pgRecoveryTarget *rt,
					   pgRestoreParams *params, bool no_sync)
{
	int			i = 0;
	int			j = 0;
	parray	   *backups = NULL;
	pgBackup   *tmp_backup = NULL;
	pgBackup   *current_backup = NULL;
	pgBackup   *dest_backup = NULL;
	pgBackup   *base_full_backup = NULL;
	pgBackup   *corrupted_backup = NULL;
	char	   *action = params->is_restore ? "Restore":"Validate";
	parray	   *parent_chain = NULL;
	parray	   *dbOid_exclude_list = NULL;
	bool        pgdata_is_empty = true;
	bool        cleanup_pgdata = false;
	bool        backup_has_tblspc = true; /* backup contain tablespace */
	XLogRecPtr  shift_lsn = InvalidXLogRecPtr;

	if (instanceState == NULL)
		elog(ERROR, "required parameter not specified: --instance");

	if (params->is_restore)
	{
		if (instance_config.pgdata == NULL)
			elog(ERROR,
				"required parameter not specified: PGDATA (-D, --pgdata)");

		/* Check if restore destination empty */
		if (!dir_is_empty(instance_config.pgdata, FIO_DB_HOST))
		{
			/* if destination directory is empty, then incremental restore may be disabled */
			pgdata_is_empty = false;

			/* Check that remote system is NOT running and systemd id is the same as ours */
			if (params->incremental_mode != INCR_NONE)
			{
				DestDirIncrCompatibility rc;
				bool ok_to_go = true;

				elog(INFO, "Running incremental restore into nonempty directory: \"%s\"",
					 instance_config.pgdata);

				rc = check_incremental_compatibility(instance_config.pgdata,
													 instance_config.system_identifier,
													 params->incremental_mode);
				if (rc == POSTMASTER_IS_RUNNING)
				{
					/* Even with force flag it is unwise to run
					 * incremental restore over running instance
					 */
					ok_to_go = false;
				}
				else if (rc == SYSTEM_ID_MISMATCH)
				{
					/*
					 * In force mode it is possible to ignore system id mismatch
					 * by just wiping clean the destination directory.
					 */
					if (params->incremental_mode != INCR_NONE && params->force)
						cleanup_pgdata = true;
					else
						ok_to_go = false;
				}
				else if (rc == BACKUP_LABEL_EXISTS)
				{
					/*
					 * A big no-no for lsn-based incremental restore
					 * If there is backup label in PGDATA, then this cluster was probably
					 * restored from backup, but not started yet. Which means that values
					 * in pg_control are not synchronized with PGDATA and so we cannot use
					 * incremental restore in LSN mode, because it is relying on pg_control
					 * to calculate switchpoint.
					 */
					if (params->incremental_mode == INCR_LSN)
						ok_to_go = false;
				}
				else if (rc == DEST_IS_NOT_OK)
				{
					/*
					 * Something else is wrong. For example, postmaster.pid is mangled,
					 * so we cannot be sure that postmaster is running or not.
					 * It is better to just error out.
					 */
					ok_to_go = false;
				}

				if (!ok_to_go)
					elog(ERROR, "Incremental restore is not allowed");
			}
			else
				elog(ERROR, "Restore destination is not empty: \"%s\"",
					 instance_config.pgdata);
		}
	}

	elog(LOG, "%s begin.", action);

	/* Get list of all backups sorted in order of descending start time */
	backups = catalog_get_backup_list(instanceState, INVALID_BACKUP_ID);

	/* Find backup range we should restore or validate. */
	while ((i < parray_num(backups)) && !dest_backup)
	{
		current_backup = (pgBackup *) parray_get(backups, i);
		i++;

		/* Skip all backups which started after target backup */
		if (target_backup_id && current_backup->start_time > target_backup_id)
			continue;

		/*
		 * [PGPRO-1164] If BACKUP_ID is not provided for restore command,
		 *  we must find the first valid(!) backup.

		 * If target_backup_id is not provided, we can be sure that
		 * PITR for restore or validate is requested.
		 * So we can assume that user is more interested in recovery to specific point
		 * in time and NOT interested in revalidation of invalid backups.
		 * So based on that assumptions we should choose only OK and DONE backups
		 * as candidates for validate and restore.
		 */

		if (target_backup_id == INVALID_BACKUP_ID &&
			(current_backup->status != BACKUP_STATUS_OK &&
			 current_backup->status != BACKUP_STATUS_DONE))
		{
			elog(WARNING, "Skipping backup %s, because it has non-valid status: %s",
				base36enc(current_backup->start_time), status2str(current_backup->status));
			continue;
		}

		/*
		 * We found target backup. Check its status and
		 * ensure that it satisfies recovery target.
		 */
		if ((target_backup_id == current_backup->start_time
			|| target_backup_id == INVALID_BACKUP_ID))
		{

			/* backup is not ok,
			 * but in case of CORRUPT or ORPHAN revalidation is possible
			 * unless --no-validate is used,
			 * in other cases throw an error.
			 */
			 // 1. validate
			 // 2. validate -i INVALID_ID <- allowed revalidate
			 // 3. restore -i INVALID_ID <- allowed revalidate and restore
			 // 4. restore <- impossible
			 // 5. restore --no-validate <- forbidden
			if (current_backup->status != BACKUP_STATUS_OK &&
				current_backup->status != BACKUP_STATUS_DONE)
			{
				if ((current_backup->status == BACKUP_STATUS_ORPHAN ||
					current_backup->status == BACKUP_STATUS_CORRUPT ||
					current_backup->status == BACKUP_STATUS_RUNNING)
					&& (!params->no_validate || params->force))
					elog(WARNING, "Backup %s has status: %s",
						 base36enc(current_backup->start_time), status2str(current_backup->status));
				else
					elog(ERROR, "Backup %s has status: %s",
						 base36enc(current_backup->start_time), status2str(current_backup->status));
			}

			if (rt->target_tli)
			{
				parray	   *timelines;

			//	elog(LOG, "target timeline ID = %u", rt->target_tli);
				/* Read timeline history files from archives */
				timelines = read_timeline_history(instanceState->instance_wal_subdir_path,
												  rt->target_tli, true);

				if (!timelines)
					elog(ERROR, "Failed to get history file for target timeline %i", rt->target_tli);

				if (!satisfy_timeline(timelines, current_backup))
				{
					if (target_backup_id != INVALID_BACKUP_ID)
						elog(ERROR, "target backup %s does not satisfy target timeline",
							 base36enc(target_backup_id));
					else
						/* Try to find another backup that satisfies target timeline */
						continue;
				}

				parray_walk(timelines, pfree);
				parray_free(timelines);
			}

			if (!satisfy_recovery_target(current_backup, rt))
			{
				if (target_backup_id != INVALID_BACKUP_ID)
					elog(ERROR, "Requested backup %s does not satisfy restore options",
						 base36enc(target_backup_id));
				else
					/* Try to find another backup that satisfies target options */
					continue;
			}

			/*
			 * Backup is fine and satisfies all recovery options.
			 * Save it as dest_backup
			 */
			dest_backup = current_backup;
		}
	}

	/* TODO: Show latest possible target */
	if (dest_backup == NULL)
	{
		/* Failed to find target backup */
		if (target_backup_id)
			elog(ERROR, "Requested backup %s is not found.", base36enc(target_backup_id));
		else
			elog(ERROR, "Backup satisfying target options is not found.");
		/* TODO: check if user asked PITR or just restore of latest backup */
	}

	/* If we already found dest_backup, look for full backup. */
	if (dest_backup->backup_mode == BACKUP_MODE_FULL)
			base_full_backup = dest_backup;
	else
	{
		int result;

		result = scan_parent_chain(dest_backup, &tmp_backup);

		if (result == ChainIsBroken)
		{
			/* chain is broken, determine missing backup ID
			 * and orphinize all his descendants
			 */
			char	   *missing_backup_id;
			time_t		missing_backup_start_time;

			missing_backup_start_time = tmp_backup->parent_backup;
			missing_backup_id = base36enc_dup(tmp_backup->parent_backup);

			for (j = 0; j < parray_num(backups); j++)
			{
				pgBackup *backup = (pgBackup *) parray_get(backups, j);

				/* use parent backup start_time because he is missing
				 * and we must orphinize his descendants
				 */
				if (is_parent(missing_backup_start_time, backup, false))
				{
					if (backup->status == BACKUP_STATUS_OK ||
						backup->status == BACKUP_STATUS_DONE)
					{
						write_backup_status(backup, BACKUP_STATUS_ORPHAN, true);

						elog(WARNING, "Backup %s is orphaned because his parent %s is missing",
								base36enc(backup->start_time), missing_backup_id);
					}
					else
					{
						elog(WARNING, "Backup %s has missing parent %s",
								base36enc(backup->start_time), missing_backup_id);
					}
				}
			}
			pg_free(missing_backup_id);
			/* No point in doing futher */
			elog(ERROR, "%s of backup %s failed.", action, base36enc(dest_backup->start_time));
		}
		else if (result == ChainIsInvalid)
		{
			/* chain is intact, but at least one parent is invalid */
			set_orphan_status(backups, tmp_backup);
			tmp_backup = find_parent_full_backup(dest_backup);

			/* sanity */
			if (!tmp_backup)
				elog(ERROR, "Parent full backup for the given backup %s was not found",
						base36enc(dest_backup->start_time));
		}

		/* We have found full backup */
		base_full_backup = tmp_backup;
	}

	if (base_full_backup == NULL)
		elog(ERROR, "Full backup satisfying target options is not found.");

	/*
	 * Ensure that directories provided in tablespace mapping are valid
	 * i.e. empty or not exist.
	 */
	if (params->is_restore)
	{
		int rc = check_tablespace_mapping(dest_backup,
										  params->incremental_mode != INCR_NONE, params->force,
										  pgdata_is_empty, params->no_validate);

		/* backup contain no tablespaces */
		if (rc == NoTblspc)
			backup_has_tblspc = false;

		if (params->incremental_mode != INCR_NONE && !cleanup_pgdata && pgdata_is_empty && (rc != NotEmptyTblspc))
		{
			elog(INFO, "Destination directory and tablespace directories are empty, "
					"disable incremental restore");
			params->incremental_mode = INCR_NONE;
		}

		/* no point in checking external directories if their restore is not requested */
		//TODO:
		//		- make check_external_dir_mapping more like check_tablespace_mapping
		//		- honor force flag in case of incremental restore just like check_tablespace_mapping
		if (!params->skip_external_dirs)
			check_external_dir_mapping(dest_backup, params->incremental_mode != INCR_NONE);
	}

	/* At this point we are sure that parent chain is whole
	 * so we can build separate array, containing all needed backups,
	 * to simplify validation and restore
	 */
	parent_chain = parray_new();

	/* Take every backup that is a child of base_backup AND parent of dest_backup
	 * including base_backup and dest_backup
	 */

	tmp_backup = dest_backup;
	while (tmp_backup)
	{
		parray_append(parent_chain, tmp_backup);
		tmp_backup = tmp_backup->parent_backup_link;
	}

	/*
	 * Determine the shift-LSN
	 * Consider the example A:
	 *
	 *
	 *              /----D----------F->
	 * -A--B---C---*-------X----->
	 *
	 * [A,F] - incremental chain
	 * X - the state of pgdata
	 * F - destination backup
	 * * - switch point
	 *
	 * When running incremental restore in 'lsn' mode, we get a bitmap of pages,
	 * whose LSN is less than shift-LSN (backup C stop_lsn).
	 * So when restoring file, we can skip restore of pages coming from
	 * A, B and C.
	 * Pages from D and F cannot be skipped due to incremental restore.
	 *
	 * Consider the example B:
	 *
	 *
	 *      /----------X---->
	 * ----*---A---B---C-->
	 *
	 * [A,C] - incremental chain
	 * X - the state of pgdata
	 * C - destination backup
	 * * - switch point
	 *
	 * Incremental restore in shift mode IS NOT POSSIBLE in this case.
	 * We must be able to differentiate the scenario A and scenario B.
	 *
	 */
	if (params->is_restore && params->incremental_mode == INCR_LSN)
	{
		RedoParams redo;
		parray	  *timelines = NULL;

		/* [Issue #313] check for previous failed incremental restore */
		{
			char    filename[MAXPGPATH];

			join_path_components(filename, instance_config.pgdata, XLOG_CONTROL_BAK_FILE);
			if (fio_access(filename, F_OK, FIO_DB_HOST) == 0)
			{
				elog(WARNING, "\"%s\" found, using incremental restore parameters from it", filename);
				get_redo(instance_config.pgdata, XLOG_CONTROL_BAK_FILE, &redo);
			}
			else
				get_redo(instance_config.pgdata, XLOG_CONTROL_FILE, &redo);
		}

		if (redo.checksum_version == 0)
			elog(ERROR, "Incremental restore in 'lsn' mode require "
				"data_checksums to be enabled in destination data directory");

		timelines = read_timeline_history(instanceState->instance_wal_subdir_path,
										  redo.tli, false);

		if (!timelines)
			elog(WARNING, "Failed to get history for redo timeline %i, "
				"multi-timeline incremental restore in 'lsn' mode is impossible", redo.tli);

		tmp_backup = dest_backup;

		while (tmp_backup)
		{
			/* Candidate, whose stop_lsn if less than shift LSN, is found */
			if (tmp_backup->stop_lsn < redo.lsn)
			{
				/* if candidate timeline is the same as redo TLI,
				 * then we are good to go.
				 */
				if (redo.tli == tmp_backup->tli)
				{
					elog(INFO, "Backup %s is chosen as shiftpoint, its Stop LSN will be used as shift LSN",
						base36enc(tmp_backup->start_time));

					shift_lsn = tmp_backup->stop_lsn;
					break;
				}

				if (!timelines)
				{
					elog(WARNING, "Redo timeline %i differs from target timeline %i, "
						"in this case, to safely run incremental restore in 'lsn' mode, "
						"the history file for timeline %i is mandatory",
						redo.tli, tmp_backup->tli, redo.tli);
					break;
				}

				/* check whether the candidate tli is a part of redo TLI history */
				if (tliIsPartOfHistory(timelines, tmp_backup->tli))
				{
					shift_lsn = tmp_backup->stop_lsn;
					break;
				}
				else
					elog(INFO, "Backup %s cannot be a shiftpoint, "
							"because its tli %i is not in history of redo timeline %i",
						base36enc(tmp_backup->start_time), tmp_backup->tli, redo.tli);
			}

			tmp_backup = tmp_backup->parent_backup_link;
		}

		if (XLogRecPtrIsInvalid(shift_lsn))
			elog(ERROR, "Cannot perform incremental restore of backup chain %s in 'lsn' mode, "
						"because destination directory redo point %X/%X on tli %i is out of reach",
					base36enc(dest_backup->start_time),
					(uint32) (redo.lsn >> 32), (uint32) redo.lsn, redo.tli);
		else
			elog(INFO, "Destination directory redo point %X/%X on tli %i is "
					"within reach of backup %s with Stop LSN %X/%X on tli %i",
				(uint32) (redo.lsn >> 32), (uint32) redo.lsn, redo.tli,
				base36enc(tmp_backup->start_time),
				(uint32) (tmp_backup->stop_lsn >> 32), (uint32) tmp_backup->stop_lsn,
				tmp_backup->tli);

		elog(INFO, "shift LSN: %X/%X",
			(uint32) (shift_lsn >> 32), (uint32) shift_lsn);

	}
	params->shift_lsn = shift_lsn;

	/* for validation or restore with enabled validation */
	if (!params->is_restore || !params->no_validate)
	{
		if (dest_backup->backup_mode != BACKUP_MODE_FULL)
			elog(INFO, "Validating parents for backup %s", base36enc(dest_backup->start_time));

		/*
		 * Validate backups from base_full_backup to dest_backup.
		 */
		for (i = parray_num(parent_chain) - 1; i >= 0; i--)
		{
			tmp_backup = (pgBackup *) parray_get(parent_chain, i);

			/* lock every backup in chain in read-only mode */
			if (!lock_backup(tmp_backup, true, false))
			{
				elog(ERROR, "Cannot lock backup %s directory",
					 base36enc(tmp_backup->start_time));
			}

			/* validate datafiles only */
			pgBackupValidate(tmp_backup, params);

			/* After pgBackupValidate() only following backup
			 * states are possible: ERROR, RUNNING, CORRUPT and OK.
			 * Validate WAL only for OK, because there is no point
			 * in WAL validation for corrupted, errored or running backups.
			 */
			if (tmp_backup->status != BACKUP_STATUS_OK)
			{
				corrupted_backup = tmp_backup;
				break;
			}
			/* We do not validate WAL files of intermediate backups
			 * It`s done to speed up restore
			 */
		}

		/* There is no point in wal validation of corrupted backups */
		// TODO: there should be a way for a user to request only(!) WAL validation
		if (!corrupted_backup)
		{
			/*
			 * Validate corresponding WAL files.
			 * We pass base_full_backup timeline as last argument to this function,
			 * because it's needed to form the name of xlog file.
			 */
			validate_wal(dest_backup, instanceState->instance_wal_subdir_path, rt->target_time,
						 rt->target_xid, rt->target_lsn,
						 dest_backup->tli, instance_config.xlog_seg_size);
		}
		/* Orphanize every OK descendant of corrupted backup */
		else
			set_orphan_status(backups, corrupted_backup);
	}

	/*
	 * If dest backup is corrupted or was orphaned in previous check
	 * produce corresponding error message
	 */
	if (dest_backup->status == BACKUP_STATUS_OK ||
		dest_backup->status == BACKUP_STATUS_DONE)
	{
		if (params->no_validate)
			elog(WARNING, "Backup %s is used without validation.", base36enc(dest_backup->start_time));
		else
			elog(INFO, "Backup %s is valid.", base36enc(dest_backup->start_time));
	}
	else if (dest_backup->status == BACKUP_STATUS_CORRUPT)
	{
		if (params->force)
			elog(WARNING, "Backup %s is corrupt.", base36enc(dest_backup->start_time));
		else
			elog(ERROR, "Backup %s is corrupt.", base36enc(dest_backup->start_time));
	}
	else if (dest_backup->status == BACKUP_STATUS_ORPHAN)
	{
		if (params->force)
			elog(WARNING, "Backup %s is orphan.", base36enc(dest_backup->start_time));
		else
			elog(ERROR, "Backup %s is orphan.", base36enc(dest_backup->start_time));
	}
	else
		elog(ERROR, "Backup %s has status: %s",
				base36enc(dest_backup->start_time), status2str(dest_backup->status));

	/* We ensured that all backups are valid, now restore if required
	 */
	if (params->is_restore)
	{
		/*
		 * Get a list of dbOids to skip if user requested the partial restore.
		 * It is important that we do this after(!) validation so
		 * database_map can be trusted.
		 * NOTE: database_map could be missing for legal reasons, e.g. missing
		 * permissions on pg_database during `backup` and, as long as user
		 * do not request partial restore, it`s OK.
		 *
		 * If partial restore is requested and database map doesn't exist,
		 * throw an error.
		 */
		if (params->partial_db_list)
			dbOid_exclude_list = get_dbOid_exclude_list(dest_backup, params->partial_db_list,
														params->partial_restore_type);

		if (rt->lsn_string &&
			parse_server_version(dest_backup->server_version) < 100000)
			elog(ERROR, "Backup %s was created for version %s which doesn't support recovery_target_lsn",
					 base36enc(dest_backup->start_time),
					 dest_backup->server_version);

		restore_chain(dest_backup, parent_chain, dbOid_exclude_list, params,
					  instance_config.pgdata, no_sync, cleanup_pgdata, backup_has_tblspc);

		//TODO rename and update comment
		/* Create recovery.conf with given recovery target parameters */
		create_recovery_conf(instanceState, target_backup_id, rt, dest_backup, params);
	}

	/* ssh connection to longer needed */
	fio_disconnect();

	elog(INFO, "%s of backup %s completed.",
		 action, base36enc(dest_backup->start_time));

	/* cleanup */
	parray_walk(backups, pgBackupFree);
	parray_free(backups);
	parray_free(parent_chain);

	return 0;
}

/*
 * Restore backup chain.
 * Flag 'cleanup_pgdata' demands the removing of already existing content in PGDATA.
 */
void
restore_chain(pgBackup *dest_backup, parray *parent_chain,
			  parray *dbOid_exclude_list, pgRestoreParams *params,
			  const char *pgdata_path, bool no_sync, bool cleanup_pgdata,
			  bool backup_has_tblspc)
{
	int			i;
	char		timestamp[100];
	parray      *pgdata_files = NULL;
	parray		*dest_files = NULL;
	parray		*external_dirs = NULL;
	pgFile	*dest_pg_control_file = NULL;
	char	dest_pg_control_fullpath[MAXPGPATH];
	char	dest_pg_control_bak_fullpath[MAXPGPATH];
	/* arrays with meta info for multi threaded backup */
	pthread_t  *threads;
	restore_files_arg *threads_args;
	bool		restore_isok = true;
	bool        use_bitmap = true;

	/* fancy reporting */
	char		pretty_dest_bytes[20];
	char		pretty_total_bytes[20];
	size_t		dest_bytes = 0;
	size_t		total_bytes = 0;
	char		pretty_time[20];
	time_t		start_time, end_time;

	/* Preparations for actual restoring */
	time2iso(timestamp, lengthof(timestamp), dest_backup->start_time, false);
	elog(INFO, "Restoring the database from backup at %s", timestamp);

	dest_files = get_backup_filelist(dest_backup, true);

	/* Lock backup chain and make sanity checks */
	for (i = parray_num(parent_chain) - 1; i >= 0; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(parent_chain, i);

		if (!lock_backup(backup, true, false))
			elog(ERROR, "Cannot lock backup %s", base36enc(backup->start_time));

		if (backup->status != BACKUP_STATUS_OK &&
			backup->status != BACKUP_STATUS_DONE)
		{
			if (params->force)
				elog(WARNING, "Backup %s is not valid, restore is forced",
					 base36enc(backup->start_time));
			else
				elog(ERROR, "Backup %s cannot be restored because it is not valid",
					 base36enc(backup->start_time));
		}

		/* confirm block size compatibility */
		if (backup->block_size != BLCKSZ)
			elog(ERROR,
				"BLCKSZ(%d) is not compatible(%d expected)",
				backup->block_size, BLCKSZ);

		if (backup->wal_block_size != XLOG_BLCKSZ)
			elog(ERROR,
				"XLOG_BLCKSZ(%d) is not compatible(%d expected)",
				backup->wal_block_size, XLOG_BLCKSZ);

		/* populate backup filelist */
		if (backup->start_time != dest_backup->start_time)
			backup->files = get_backup_filelist(backup, true);
		else
			backup->files = dest_files;

		/*
		 * this sorting is important, because we rely on it to find
		 * destination file in intermediate backups file lists
		 * using bsearch.
		 */
		parray_qsort(backup->files, pgFileCompareRelPathWithExternal);
	}

	/* If dest backup version is older than 2.4.0, then bitmap optimization
	 * is impossible to use, because bitmap restore rely on pgFile.n_blocks,
	 * which is not always available in old backups.
	 */
	if (parse_program_version(dest_backup->program_version) < 20400)
	{
		use_bitmap = false;

		if (params->incremental_mode != INCR_NONE)
			elog(ERROR, "incremental restore is not possible for backups older than 2.3.0 version");
	}

	/* There is no point in bitmap restore, when restoring a single FULL backup,
	 * unless we are running incremental-lsn restore, then bitmap is mandatory.
	 */
	if (use_bitmap && parray_num(parent_chain) == 1)
		use_bitmap = params->incremental_mode != INCR_NONE;

	/*
	 * Restore dest_backup internal directories.
	 */
	create_data_directories(dest_files, instance_config.pgdata,
							dest_backup->root_dir, backup_has_tblspc,
							params->incremental_mode != INCR_NONE,
							FIO_DB_HOST);

	/*
	 * Restore dest_backup external directories.
	 */
	if (dest_backup->external_dir_str && !params->skip_external_dirs)
	{
		external_dirs = make_external_directory_list(dest_backup->external_dir_str, true);

		if (!external_dirs)
			elog(ERROR, "Failed to get a list of external directories");

		if (parray_num(external_dirs) > 0)
			elog(LOG, "Restore external directories");

		for (i = 0; i < parray_num(external_dirs); i++)
			fio_mkdir(parray_get(external_dirs, i),
					  DIR_PERMISSION, FIO_DB_HOST);
	}

	/*
	 * Setup directory structure for external directories
	 */
	for (i = 0; i < parray_num(dest_files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(dest_files, i);

		if (S_ISDIR(file->mode))
			total_bytes += 4096;

		if (!params->skip_external_dirs &&
			file->external_dir_num && S_ISDIR(file->mode))
		{
			char	   *external_path;
			char		dirpath[MAXPGPATH];

			if (parray_num(external_dirs) < file->external_dir_num - 1)
				elog(ERROR, "Inconsistent external directory backup metadata");

			external_path = parray_get(external_dirs, file->external_dir_num - 1);
			join_path_components(dirpath, external_path, file->rel_path);

			elog(VERBOSE, "Create external directory \"%s\"", dirpath);
			fio_mkdir(dirpath, file->mode, FIO_DB_HOST);
		}
	}

	/* setup threads */
	pfilearray_clear_locks(dest_files);

	/* Get list of files in destination directory and remove redundant files */
	if (params->incremental_mode != INCR_NONE || cleanup_pgdata)
	{
		pgdata_files = parray_new();

		elog(INFO, "Extracting the content of destination directory for incremental restore");

		time(&start_time);
		fio_list_dir(pgdata_files, pgdata_path, false, true, false, false, true, 0);

		/*
		 * TODO:
		 * 1. Currently we are cleaning the tablespaces in check_tablespace_mapping and PGDATA here.
		 *    It would be great to do all this work in one place.
		 * 
		 * 2. In case of tablespace remapping we do not cleanup the old tablespace path,
		 *    it is just left as it is.
		 *    Lookup tests.incr_restore.IncrRestoreTest.test_incr_restore_with_tablespace_5
		 */

		/* get external dirs content */
		if (external_dirs)
		{
			for (i = 0; i < parray_num(external_dirs); i++)
			{
				char *external_path = parray_get(external_dirs, i);
				parray	*external_files = parray_new();

				fio_list_dir(external_files, external_path,
							 false, true, false, false, true, i+1);

				parray_concat(pgdata_files, external_files);
				parray_free(external_files);
			}
		}

		parray_qsort(pgdata_files, pgFileCompareRelPathWithExternalDesc);

		time(&end_time);
		pretty_time_interval(difftime(end_time, start_time),
							 pretty_time, lengthof(pretty_time));

		elog(INFO, "Destination directory content extracted, time elapsed: %s",
				pretty_time);

		elog(INFO, "Removing redundant files in destination directory");
		time(&start_time);
		for (i = 0; i < parray_num(pgdata_files); i++)
		{
			bool     redundant = true;
			pgFile	*file = (pgFile *) parray_get(pgdata_files, i);

			if (parray_bsearch(dest_backup->files, file, pgFileCompareRelPathWithExternal))
				redundant = false;

			/* pg_filenode.map are always restored, because it's crc cannot be trusted */
			if (file->external_dir_num == 0 &&
				pg_strcasecmp(file->name, RELMAPPER_FILENAME) == 0)
				redundant = true;

			/* global/pg_control.pbk.bak are always keeped, because it's needed for restart failed incremental restore */
			if (file->external_dir_num == 0 &&
				pg_strcasecmp(file->rel_path, XLOG_CONTROL_BAK_FILE) == 0)
				redundant = false;

			/* do not delete the useful internal directories */
			if (S_ISDIR(file->mode) && !redundant)
				continue;

			/* if file does not exists in destination list, then we can safely unlink it */
			if (cleanup_pgdata || redundant)
			{
				char		fullpath[MAXPGPATH];

				join_path_components(fullpath, pgdata_path, file->rel_path);

				fio_delete(file->mode, fullpath, FIO_DB_HOST);
				elog(VERBOSE, "Deleted file \"%s\"", fullpath);

				/* shrink pgdata list */
				pgFileFree(file);
				parray_remove(pgdata_files, i);
				i--;
			}
		}

		if (cleanup_pgdata)
		{
			/* Destination PGDATA and tablespaces were cleaned up, so it's the regular restore from this point */
			params->incremental_mode = INCR_NONE;
			parray_free(pgdata_files);
			pgdata_files = NULL;
		}

		time(&end_time);
		pretty_time_interval(difftime(end_time, start_time),
							 pretty_time, lengthof(pretty_time));

		/* At this point PDATA do not contain files, that do not exists in dest backup file list */
		elog(INFO, "Redundant files are removed, time elapsed: %s", pretty_time);
	}

	/*
	 * Close ssh connection belonging to the main thread
	 * to avoid the possibility of been killed for idleness
	 */
	fio_disconnect();

	threads = (pthread_t *) palloc(sizeof(pthread_t) * num_threads);
	threads_args = (restore_files_arg *) palloc(sizeof(restore_files_arg) *
												num_threads);
	if (dest_backup->stream)
		dest_bytes = dest_backup->pgdata_bytes + dest_backup->wal_bytes;
	else
		dest_bytes = dest_backup->pgdata_bytes;

	pretty_size(dest_bytes, pretty_dest_bytes, lengthof(pretty_dest_bytes));

	/*
	 * [Issue #313]
	 * find pg_control file (in already sorted earlier dest_files, see parray_qsort(backup->files...))
	 * and exclude it from list for future special processing
	 */
	{
		int control_file_elem_index;
		pgFile search_key;
		MemSet(&search_key, 0, sizeof(pgFile));
		/* pgFileCompareRelPathWithExternal uses only .rel_path and .external_dir_num for comparision */
		search_key.rel_path = XLOG_CONTROL_FILE;
		search_key.external_dir_num = 0;
		control_file_elem_index = parray_bsearch_index(dest_files, &search_key, pgFileCompareRelPathWithExternal);

		if (control_file_elem_index < 0)
			elog(ERROR, "File \"%s\" not found in backup %s", XLOG_CONTROL_FILE, base36enc(dest_backup->start_time));
		dest_pg_control_file = parray_remove(dest_files, control_file_elem_index);

		join_path_components(dest_pg_control_fullpath, pgdata_path, XLOG_CONTROL_FILE);
		join_path_components(dest_pg_control_bak_fullpath, pgdata_path, XLOG_CONTROL_BAK_FILE);
		/*
		 * rename (if it exist) dest control file before restoring
		 * if it doesn't exist, that mean, that we already restoring in a previously failed
		 * pgdata, where XLOG_CONTROL_BAK_FILE exist
		 */
		if (params->incremental_mode != INCR_NONE
			&& fio_access(dest_pg_control_fullpath, F_OK, FIO_DB_HOST) == 0)
			fio_rename(dest_pg_control_fullpath, dest_pg_control_bak_fullpath, FIO_DB_HOST);
	}

	elog(INFO, "Start restoring backup files. PGDATA size: %s", pretty_dest_bytes);
	time(&start_time);
	thread_interrupted = false;

	/* Restore files into target directory */
	for (i = 0; i < num_threads; i++)
	{
		threads_args[i] = (restore_files_arg){
			.dest_files = dest_files,
			.pgdata_files = pgdata_files,
			.dest_backup = dest_backup,
			.dest_external_dirs = external_dirs,
			.parent_chain = parent_chain,
			.dbOid_exclude_list = dbOid_exclude_list,
			.skip_external_dirs = params->skip_external_dirs,
			.to_root = pgdata_path,
			.use_bitmap = use_bitmap,
			.incremental_mode = params->incremental_mode,
			.shift_lsn = params->shift_lsn,
			.restored_bytes = 0,
			/* By default there are some error */
			.ret = 1,
		};

		/* Useless message TODO: rewrite */
		elog(LOG, "Start thread %i", i + 1);

		pthread_create(&threads[i], NULL, restore_files, &(threads_args[i]));
	}

	/* Wait threads */
	for (i = 0; i < num_threads; i++)
	{
		pthread_join(threads[i], NULL);
		if (threads_args[i].ret == 1)
			restore_isok = false;

		total_bytes += threads_args[i].restored_bytes;
	}

	/* [Issue #313] copy pg_control at very end */
	if (restore_isok)
	{
		total_bytes += restore_file(dest_pg_control_file, dest_pg_control_fullpath, false, NULL,
			dest_backup, parent_chain, use_bitmap, params->incremental_mode, params->shift_lsn);
		if (params->incremental_mode != INCR_NONE)
			fio_unlink(dest_pg_control_bak_fullpath, FIO_DB_HOST);
	}

	time(&end_time);
	pretty_time_interval(difftime(end_time, start_time),
						 pretty_time, lengthof(pretty_time));
	pretty_size(total_bytes, pretty_total_bytes, lengthof(pretty_total_bytes));

	if (restore_isok)
	{
		elog(INFO, "Backup files are restored. Transfered bytes: %s, time elapsed: %s",
			pretty_total_bytes, pretty_time);

		elog(INFO, "Restore incremental ratio (less is better): %.f%% (%s/%s)",
			((float) total_bytes / dest_bytes) * 100,
			pretty_total_bytes, pretty_dest_bytes);
	}
	else
		elog(ERROR, "Backup files restoring failed. Transfered bytes: %s, time elapsed: %s",
			pretty_total_bytes, pretty_time);

	/* Close page header maps */
	for (i = parray_num(parent_chain) - 1; i >= 0; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(parent_chain, i);
		cleanup_header_map(&(backup->hdr_map));
	}

	if (no_sync)
		elog(WARNING, "Restored files are not synced to disk");
	else
	{
		elog(INFO, "Syncing restored files to disk");
		time(&start_time);

		for (i = 0; i < parray_num(dest_files); i++)
		{
			char		to_fullpath[MAXPGPATH];
			pgFile	   *dest_file = (pgFile *) parray_get(dest_files, i);

			if (S_ISDIR(dest_file->mode))
				continue;

			/* skip external files if ordered to do so */
			if (dest_file->external_dir_num > 0 &&
				params->skip_external_dirs)
				continue;

			/* construct fullpath */
			if (dest_file->external_dir_num == 0)
			{
				if (strcmp(PG_TABLESPACE_MAP_FILE, dest_file->rel_path) == 0)
					continue;
				if (strcmp(DATABASE_MAP, dest_file->rel_path) == 0)
					continue;
				join_path_components(to_fullpath, pgdata_path, dest_file->rel_path);
			}
			else
			{
				char *external_path = parray_get(external_dirs, dest_file->external_dir_num - 1);
				join_path_components(to_fullpath, external_path, dest_file->rel_path);
			}

			/* TODO: write test for case: file to be synced is missing */
			if (fio_sync(to_fullpath, FIO_DB_HOST) != 0)
				elog(ERROR, "Failed to sync file \"%s\": %s", to_fullpath, strerror(errno));
		}

		/* sync control file */
		if (fio_sync(dest_pg_control_fullpath, FIO_DB_HOST) != 0)
			elog(ERROR, "Failed to sync file \"%s\": %s", dest_pg_control_fullpath, strerror(errno));

		time(&end_time);
		pretty_time_interval(difftime(end_time, start_time),
							 pretty_time, lengthof(pretty_time));
		elog(INFO, "Restored backup files are synced, time elapsed: %s", pretty_time);
	}

	/* cleanup */
	pfree(threads);
	pfree(threads_args);

	if (external_dirs != NULL)
		free_dir_list(external_dirs);

	if (pgdata_files)
	{
		parray_walk(pgdata_files, pgFileFree);
		parray_free(pgdata_files);
	}

	pgFileFree(dest_pg_control_file);

	for (i = parray_num(parent_chain) - 1; i >= 0; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(parent_chain, i);

		parray_walk(backup->files, pgFileFree);
		parray_free(backup->files);
	}
}

/*
 * Restore files into $PGDATA.
 */
static void *
restore_files(void *arg)
{
	int         i;
	uint64      n_files;
	char        to_fullpath[MAXPGPATH];
	char       *out_buf = pgut_malloc(STDIO_BUFSIZE);

	restore_files_arg *arguments = (restore_files_arg *) arg;

	n_files = (unsigned long) parray_num(arguments->dest_files);

	for (i = 0; i < parray_num(arguments->dest_files); i++)
	{
		bool     already_exists = false;
		pgFile	*dest_file = (pgFile *) parray_get(arguments->dest_files, i);

		/* Directories were created before */
		if (S_ISDIR(dest_file->mode))
			continue;

		if (!pg_atomic_test_set_flag(&dest_file->lock))
			continue;

		/* check for interrupt */
		if (interrupted || thread_interrupted)
			elog(ERROR, "Interrupted during restore");

		if (progress)
			elog(INFO, "Progress: (%d/%lu). Restore file \"%s\"",
				 i + 1, n_files, dest_file->rel_path);

		/* Only files from pgdata can be skipped by partial restore */
		if (arguments->dbOid_exclude_list && dest_file->external_dir_num == 0)
		{
			/* Check if the file belongs to the database we exclude */
			if (parray_bsearch(arguments->dbOid_exclude_list,
							   &dest_file->dbOid, pgCompareOid))
			{
				/*
				 * We cannot simply skip the file, because it may lead to
				 * failure during WAL redo; hence, create empty file.
				 */
				create_empty_file(FIO_BACKUP_HOST,
					  arguments->to_root, FIO_DB_HOST, dest_file);

				elog(VERBOSE, "Skip file due to partial restore: \"%s\"",
						dest_file->rel_path);
				continue;
			}
		}

		/* Do not restore tablespace_map file */
		if ((dest_file->external_dir_num == 0) &&
			strcmp(PG_TABLESPACE_MAP_FILE, dest_file->rel_path) == 0)
		{
			elog(VERBOSE, "Skip tablespace_map");
			continue;
		}

		/* Do not restore database_map file */
		if ((dest_file->external_dir_num == 0) &&
			strcmp(DATABASE_MAP, dest_file->rel_path) == 0)
		{
			elog(VERBOSE, "Skip database_map");
			continue;
		}

		/* Do no restore external directory file if a user doesn't want */
		if (arguments->skip_external_dirs && dest_file->external_dir_num > 0)
			continue;

		/* set fullpath of destination file */
		if (dest_file->external_dir_num == 0)
			join_path_components(to_fullpath, arguments->to_root, dest_file->rel_path);
		else
		{
			char	*external_path = parray_get(arguments->dest_external_dirs,
												dest_file->external_dir_num - 1);
			join_path_components(to_fullpath, external_path, dest_file->rel_path);
		}

		if (arguments->incremental_mode != INCR_NONE &&
			parray_bsearch(arguments->pgdata_files, dest_file, pgFileCompareRelPathWithExternalDesc))
		{
			already_exists = true;
		}

		arguments->restored_bytes += restore_file(dest_file, to_fullpath, already_exists, out_buf,
			arguments->dest_backup, arguments->parent_chain, arguments->use_bitmap,
			arguments->incremental_mode, arguments->shift_lsn);
	}

	free(out_buf);

	/* ssh connection to longer needed */
	fio_disconnect();

	/* Data files restoring is successful */
	arguments->ret = 0;

	return NULL;
}

/*
 * Restore one file into $PGDATA.
 */
static size_t
restore_file(pgFile *dest_file, const char *to_fullpath, bool already_exists, char *out_buf,
	pgBackup *dest_backup, parray *parent_chain, bool use_bitmap, IncrRestoreMode incremental_mode, XLogRecPtr shift_lsn)
{
	FILE       *out = NULL;
	size_t		restored_bytes = 0;
	PageState      *checksum_map = NULL; /* it should take ~1.5MB at most */
	datapagemap_t  *lsn_map = NULL;      /* it should take 16kB at most */
	char           *errmsg = NULL;       /* remote agent error message */

	/*
	 * Handle incremental restore case for data files.
	 * If file is already exists in pgdata, then
	 * we scan it block by block and get
	 * array of checksums for every page.
	 */
	if (already_exists &&
		dest_file->is_datafile && !dest_file->is_cfs &&
		dest_file->n_blocks > 0)
	{
		if (incremental_mode == INCR_LSN)
		{
			lsn_map = fio_get_lsn_map(to_fullpath, dest_backup->checksum_version,
							dest_file->n_blocks, shift_lsn,
							dest_file->segno * RELSEG_SIZE, FIO_DB_HOST);
		}
		else if (incremental_mode == INCR_CHECKSUM)
		{
			checksum_map = fio_get_checksum_map(to_fullpath, dest_backup->checksum_version,
												dest_file->n_blocks, dest_backup->stop_lsn,
												dest_file->segno * RELSEG_SIZE, FIO_DB_HOST);
		}
	}

	/*
	 * Open dest file and truncate it to zero, if destination
	 * file already exists and dest file size is zero, or
	 * if file do not exist
	 */
	if ((already_exists && dest_file->write_size == 0) || !already_exists)
		out = fio_fopen(to_fullpath, PG_BINARY_W, FIO_DB_HOST);
	/*
	 * If file already exists and dest size is not zero,
	 * then open it for reading and writing.
	 */
	else
		out = fio_fopen(to_fullpath, PG_BINARY_R "+", FIO_DB_HOST);

	if (out == NULL)
		elog(ERROR, "Cannot open restore target file \"%s\": %s",
			 to_fullpath, strerror(errno));

	/* update file permission */
	if (fio_chmod(to_fullpath, dest_file->mode, FIO_DB_HOST) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", to_fullpath,
			 strerror(errno));

	if (!dest_file->is_datafile || dest_file->is_cfs)
		elog(VERBOSE, "Restoring nonedata file: \"%s\"", to_fullpath);
	else
		elog(VERBOSE, "Restoring data file: \"%s\"", to_fullpath);

	// If destination file is 0 sized, then just close it and go for the next
	if (dest_file->write_size != 0)
	{
		/* Restore destination file */
		if (dest_file->is_datafile && !dest_file->is_cfs)
		{
			/* enable stdio buffering for local destination data file */
			if (!fio_is_remote_file(out) && out_buf != NULL)
				setvbuf(out, out_buf, _IOFBF, STDIO_BUFSIZE);
			/* Destination file is data file */
			restored_bytes += restore_data_file(parent_chain,
														   dest_file, out, to_fullpath,
														   use_bitmap, checksum_map,
														   shift_lsn, lsn_map, true);
		}
		else
		{
			/* disable stdio buffering for local destination nonedata file */
			if (!fio_is_remote_file(out))
				setvbuf(out, NULL, _IONBF, BUFSIZ);
			/* Destination file is nonedata file */
			restored_bytes += restore_non_data_file(parent_chain,
										dest_backup, dest_file, out, to_fullpath,
										already_exists);
		}
	}

	/* Writing is asynchronous in case of restore in remote mode, so check the agent status */
	if (fio_check_error_file(out, &errmsg))
		elog(ERROR, "Cannot write to the remote file \"%s\": %s", to_fullpath, errmsg);

	/* close file */
	if (fio_fclose(out) != 0)
		elog(ERROR, "Cannot close file \"%s\": %s", to_fullpath,
			 strerror(errno));

	if (lsn_map)
		pg_free(lsn_map->bitmap);

	pg_free(lsn_map);
	pg_free(checksum_map);

	/* free pagemap used for restore optimization */
	pg_free(dest_file->pagemap.bitmap);
	dest_file->pagemap.bitmap = NULL;

	return restored_bytes;
}

/*
 * Create recovery.conf (postgresql.auto.conf in case of PG12)
 * with given recovery target parameters
 */
static void
create_recovery_conf(InstanceState *instanceState, time_t backup_id,
					 pgRecoveryTarget *rt,
					 pgBackup *backup,
					 pgRestoreParams *params)
{
	bool		target_latest;
	bool		target_immediate;
	bool 		restore_command_provided = false;

	if (instance_config.restore_command &&
		(pg_strcasecmp(instance_config.restore_command, "none") != 0))
	{
		restore_command_provided = true;
	}

	/* restore-target='latest' support */
	target_latest = rt->target_stop != NULL &&
		strcmp(rt->target_stop, "latest") == 0;

	target_immediate = rt->target_stop != NULL &&
		strcmp(rt->target_stop, "immediate") == 0;

	/*
	 * Note that setting restore_command alone interpreted
	 * as PITR with target - "until all available WAL is replayed".
	 * We do this because of the following case:
	 * The user is restoring STREAM backup as replica but
	 * also relies on WAL archive to catch-up with master.
	 * If restore_command is provided, then it should be
	 * added to recovery config.
	 * In this scenario, "would be" replica will replay
	 * all WAL segments available in WAL archive, after that
	 * it will try to connect to master via repprotocol.
	 *
	 * The risk is obvious, what if masters current state is
	 * in "the past" relatively to latest state in the archive?
	 * We will get a replica that is "in the future" to the master.
	 * We accept this risk because its probability is low.
	 */
	if (!backup->stream || rt->time_string ||
		rt->xid_string || rt->lsn_string || rt->target_name ||
		target_immediate || target_latest || restore_command_provided)
		params->recovery_settings_mode = PITR_REQUESTED;

	elog(LOG, "----------------------------------------");

#if PG_VERSION_NUM >= 120000
	update_recovery_options(instanceState, backup, params, rt);
#else
	update_recovery_options_before_v12(instanceState, backup, params, rt);
#endif
}


/* TODO get rid of using global variables: instance_config */
static void
print_recovery_settings(InstanceState *instanceState, FILE *fp, pgBackup *backup,
							   pgRestoreParams *params, pgRecoveryTarget *rt)
{
	char restore_command_guc[16384];
	fio_fprintf(fp, "## recovery settings\n");

	/* If restore_command is provided, use it. Otherwise construct it from scratch. */
	if (instance_config.restore_command &&
		(pg_strcasecmp(instance_config.restore_command, "none") != 0))
		sprintf(restore_command_guc, "%s", instance_config.restore_command);
	else
	{
		/* default cmdline, ok for local restore */
		sprintf(restore_command_guc, "\"%s\" archive-get -B \"%s\" --instance \"%s\" "
				"--wal-file-path=%%p --wal-file-name=%%f",
				PROGRAM_FULL_PATH ? PROGRAM_FULL_PATH : PROGRAM_NAME,
				/* TODO What is going on here? Why do we use catalog path as wal-file-path? */
				instanceState->catalog_state->catalog_path, instanceState->instance_name);

		/* append --remote-* parameters provided via --archive-* settings */
		if (instance_config.archive.host)
		{
			strcat(restore_command_guc, " --remote-host=");
			strcat(restore_command_guc, instance_config.archive.host);
		}

		if (instance_config.archive.port)
		{
			strcat(restore_command_guc, " --remote-port=");
			strcat(restore_command_guc, instance_config.archive.port);
		}

		if (instance_config.archive.user)
		{
			strcat(restore_command_guc, " --remote-user=");
			strcat(restore_command_guc, instance_config.archive.user);
		}
	}

	/*
	 * We've already checked that only one of the four following mutually
	 * exclusive options is specified, so the order of calls is insignificant.
	 */
	if (rt->target_name)
		fio_fprintf(fp, "recovery_target_name = '%s'\n", rt->target_name);

	if (rt->time_string)
		fio_fprintf(fp, "recovery_target_time = '%s'\n", rt->time_string);

	if (rt->xid_string)
		fio_fprintf(fp, "recovery_target_xid = '%s'\n", rt->xid_string);

	if (rt->lsn_string)
		fio_fprintf(fp, "recovery_target_lsn = '%s'\n", rt->lsn_string);

	if (rt->target_stop && (strcmp(rt->target_stop, "immediate") == 0))
		fio_fprintf(fp, "recovery_target = '%s'\n", rt->target_stop);

	if (rt->inclusive_specified)
		fio_fprintf(fp, "recovery_target_inclusive = '%s'\n",
				rt->target_inclusive ? "true" : "false");

	if (rt->target_tli)
		fio_fprintf(fp, "recovery_target_timeline = '%u'\n", rt->target_tli);
	else
	{
#if PG_VERSION_NUM >= 120000

			/*
			 * In PG12 default recovery target timeline was changed to 'latest', which
			 * is extremely risky. Explicitly preserve old behavior of recovering to current
			 * timneline for PG12.
			 */
			fio_fprintf(fp, "recovery_target_timeline = 'current'\n");
#endif
	}

	if (rt->target_action)
		fio_fprintf(fp, "recovery_target_action = '%s'\n", rt->target_action);
	else
		/* default recovery_target_action is 'pause' */
		fio_fprintf(fp, "recovery_target_action = '%s'\n", "pause");

	elog(LOG, "Setting restore_command to '%s'", restore_command_guc);
	fio_fprintf(fp, "restore_command = '%s'\n", restore_command_guc);
}

static void
print_standby_settings_common(FILE *fp, pgBackup *backup, pgRestoreParams *params)
{
	fio_fprintf(fp, "\n## standby settings\n");
	if (params->primary_conninfo)
		fio_fprintf(fp, "primary_conninfo = '%s'\n", params->primary_conninfo);
	else if (backup->primary_conninfo)
		fio_fprintf(fp, "primary_conninfo = '%s'\n", backup->primary_conninfo);

	if (params->primary_slot_name != NULL)
		fio_fprintf(fp, "primary_slot_name = '%s'\n", params->primary_slot_name);
}

#if PG_VERSION_NUM < 120000
static void
update_recovery_options_before_v12(InstanceState *instanceState, pgBackup *backup,
								   pgRestoreParams *params, pgRecoveryTarget *rt)
{
	FILE	   *fp;
	char		path[MAXPGPATH];

	/*
	 * If PITR is not requested and instance is not restored as replica,
	 * then recovery.conf should not be created.
	 */
	if (params->recovery_settings_mode != PITR_REQUESTED &&
		!params->restore_as_replica)
	{
		return;
	}

	elog(LOG, "update recovery settings in recovery.conf");
	join_path_components(path, instance_config.pgdata, "recovery.conf");

	fp = fio_fopen(path, "w", FIO_DB_HOST);
	if (fp == NULL)
		elog(ERROR, "cannot open file \"%s\": %s", path,
			strerror(errno));

	if (fio_chmod(path, FILE_PERMISSION, FIO_DB_HOST) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", path, strerror(errno));

	fio_fprintf(fp, "# recovery.conf generated by pg_probackup %s\n",
				PROGRAM_VERSION);

	if (params->recovery_settings_mode == PITR_REQUESTED)
		print_recovery_settings(instanceState, fp, backup, params, rt);

	if (params->restore_as_replica)
	{
		print_standby_settings_common(fp, backup, params);
		fio_fprintf(fp, "standby_mode = 'on'\n");
	}

	if (fio_fflush(fp) != 0 ||
		fio_fclose(fp))
		elog(ERROR, "cannot write file \"%s\": %s", path,
			 strerror(errno));
}
#endif

/*
 * Read postgresql.auto.conf, clean old recovery options,
 * to avoid unexpected intersections.
 * Write recovery options for this backup.
 */
#if PG_VERSION_NUM >= 120000
static void
update_recovery_options(InstanceState *instanceState, pgBackup *backup,
						pgRestoreParams *params, pgRecoveryTarget *rt)

{
	char		postgres_auto_path[MAXPGPATH];
	char		postgres_auto_path_tmp[MAXPGPATH];
	char		path[MAXPGPATH];
	FILE	   *fp = NULL;
	FILE	   *fp_tmp = NULL;
	struct stat st;
	char		current_time_str[100];
	/* postgresql.auto.conf parsing */
	char		line[16384] = "\0";
	char	   *buf = NULL;
	int		    buf_len = 0;
	int		    buf_len_max = 16384;

	elog(LOG, "update recovery settings in postgresql.auto.conf");

	time2iso(current_time_str, lengthof(current_time_str), current_time, false);

	join_path_components(postgres_auto_path, instance_config.pgdata, "postgresql.auto.conf");

	if (fio_stat(postgres_auto_path, &st, false, FIO_DB_HOST) < 0)
	{
		/* file not found is not an error case */
		if (errno != ENOENT)
			elog(ERROR, "cannot stat file \"%s\": %s", postgres_auto_path,
				 strerror(errno));
		st.st_size = 0;
	}

	/* Kludge for 0-sized postgresql.auto.conf file. TODO: make something more intelligent */
	if (st.st_size > 0)
	{
		fp = fio_open_stream(postgres_auto_path, FIO_DB_HOST);
		if (fp == NULL)
			elog(ERROR, "cannot open \"%s\": %s", postgres_auto_path, strerror(errno));
	}

	sprintf(postgres_auto_path_tmp, "%s.tmp", postgres_auto_path);
	fp_tmp = fio_fopen(postgres_auto_path_tmp, "w", FIO_DB_HOST);
	if (fp_tmp == NULL)
		elog(ERROR, "cannot open \"%s\": %s", postgres_auto_path_tmp, strerror(errno));

	while (fp && fgets(line, lengthof(line), fp))
	{
		/* ignore "include 'probackup_recovery.conf'" directive */
		if (strstr(line, "include") &&
			strstr(line, "probackup_recovery.conf"))
		{
			continue;
		}

		/* ignore already existing recovery options */
		if (strstr(line, "restore_command") ||
			strstr(line, "recovery_target"))
		{
			continue;
		}

		if (!buf)
			buf = pgut_malloc(buf_len_max);

		/* avoid buffer overflow */
		if ((buf_len + strlen(line)) >= buf_len_max)
		{
			buf_len_max += (buf_len + strlen(line)) *2;
			buf = pgut_realloc(buf, buf_len_max);
		}

		buf_len += snprintf(buf+buf_len, sizeof(line), "%s", line);
	}

	/* close input postgresql.auto.conf */
	if (fp)
		fio_close_stream(fp);

	/* Write data to postgresql.auto.conf.tmp */
	if (buf_len > 0 &&
		(fio_fwrite(fp_tmp, buf, buf_len) != buf_len))
			elog(ERROR, "Cannot write to \"%s\": %s",
					postgres_auto_path_tmp, strerror(errno));

	if (fio_fflush(fp_tmp) != 0 ||
		fio_fclose(fp_tmp))
			elog(ERROR, "Cannot write file \"%s\": %s", postgres_auto_path_tmp,
				strerror(errno));
	pg_free(buf);

	if (fio_rename(postgres_auto_path_tmp, postgres_auto_path, FIO_DB_HOST) < 0)
		elog(ERROR, "Cannot rename file \"%s\" to \"%s\": %s",
					postgres_auto_path_tmp, postgres_auto_path, strerror(errno));

	if (fio_chmod(postgres_auto_path, FILE_PERMISSION, FIO_DB_HOST) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", postgres_auto_path, strerror(errno));

	if (params)
	{
		fp = fio_fopen(postgres_auto_path, "a", FIO_DB_HOST);
		if (fp == NULL)
			elog(ERROR, "cannot open file \"%s\": %s", postgres_auto_path,
				strerror(errno));

		fio_fprintf(fp, "\n# recovery settings added by pg_probackup restore of backup %s at '%s'\n",
				base36enc(backup->start_time), current_time_str);

		if (params->recovery_settings_mode == PITR_REQUESTED)
			print_recovery_settings(instanceState, fp, backup, params, rt);

		if (params->restore_as_replica)
			print_standby_settings_common(fp, backup, params);

		if (fio_fflush(fp) != 0 ||
			fio_fclose(fp))
			elog(ERROR, "cannot write file \"%s\": %s", postgres_auto_path,
				strerror(errno));

		/*
		* Create "recovery.signal" to mark this recovery as PITR for PostgreSQL.
		* In older versions presense of recovery.conf alone was enough.
		* To keep behaviour consistent with older versions,
		* we are forced to create "recovery.signal"
		* even when only restore_command is provided.
		* Presense of "recovery.signal" by itself determine only
		* one thing: do PostgreSQL must switch to a new timeline
		* after successfull recovery or not?
		*/
		if (params->recovery_settings_mode == PITR_REQUESTED)
		{
			elog(LOG, "creating recovery.signal file");
			join_path_components(path, instance_config.pgdata, "recovery.signal");

			fp = fio_fopen(path, PG_BINARY_W, FIO_DB_HOST);
			if (fp == NULL)
				elog(ERROR, "cannot open file \"%s\": %s", path,
					strerror(errno));

			if (fio_fflush(fp) != 0 ||
				fio_fclose(fp))
				elog(ERROR, "cannot write file \"%s\": %s", path,
					strerror(errno));
		}

		if (params->restore_as_replica)
		{
			elog(LOG, "creating standby.signal file");
			join_path_components(path, instance_config.pgdata, "standby.signal");

			fp = fio_fopen(path, PG_BINARY_W, FIO_DB_HOST);
			if (fp == NULL)
				elog(ERROR, "cannot open file \"%s\": %s", path,
					strerror(errno));

			if (fio_fflush(fp) != 0 ||
				fio_fclose(fp))
				elog(ERROR, "cannot write file \"%s\": %s", path,
					strerror(errno));
		}
	}
}
#endif

/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component TLIs (the ancestor
 * timelines followed by target timeline). If we cannot find the history file,
 * assume that the timeline has no parents, and return a list of just the
 * specified timeline ID.
 * based on readTimeLineHistory() in timeline.c
 */
parray *
read_timeline_history(const char *arclog_path, TimeLineID targetTLI, bool strict)
{
	parray	   *result;
	char		path[MAXPGPATH];
	char		fline[MAXPGPATH];
	FILE	   *fd = NULL;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *last_timeline = NULL;

	/* Look for timeline history file in archlog_path */
	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path,
		targetTLI);

	/* Timeline 1 does not have a history file */
	if (targetTLI != 1)
	{
		fd = fopen(path, "rt");
		if (fd == NULL)
		{
			if (errno != ENOENT)
				elog(ERROR, "could not open file \"%s\": %s", path,
					strerror(errno));

			/* There is no history file for target timeline */
			if (strict)
				elog(ERROR, "recovery target timeline %u does not exist",
					 targetTLI);
			else
				return NULL;
		}
	}

	result = parray_new();

	/*
	 * Parse the file...
	 */
	while (fd && fgets(fline, sizeof(fline), fd) != NULL)
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			elog(ERROR,
				 "syntax error in history file: %s. Expected a numeric timeline ID.",
				   fline);
		}
		if (nfields != 3)
			elog(ERROR,
				 "syntax error in history file: %s. Expected a transaction log switchpoint location.",
				   fline);

		if (last_timeline && tli <= last_timeline->tli)
			elog(ERROR,
				   "Timeline IDs must be in increasing sequence.");

		entry = pgut_new(TimeLineHistoryEntry);
		entry->tli = tli;
		entry->end = ((uint64) switchpoint_hi << 32) | switchpoint_lo;

		last_timeline = entry;
		/* Build list with newest item first */
		parray_insert(result, 0, entry);

		/* we ignore the remainder of each line */
	}

	if (fd && (ferror(fd)))
			elog(ERROR, "Failed to read from file: \"%s\"", path);

	if (fd)
		fclose(fd);

	if (last_timeline && targetTLI <= last_timeline->tli)
		elog(ERROR, "Timeline IDs must be less than child timeline's ID.");

	/* History file is empty or corrupted */
	if (parray_num(result) == 0 && targetTLI != 1)
	{
		elog(WARNING, "History file is corrupted or missing: \"%s\"", path);
		pg_free(result);
		return NULL;
	}

	/* append target timeline */
	entry = pgut_new(TimeLineHistoryEntry);
	entry->tli = targetTLI;
	/* LSN in target timeline is valid */
	entry->end = InvalidXLogRecPtr;
	parray_insert(result, 0, entry);

	return result;
}

/* TODO: do not ignore timelines. What if requested target located in different timeline? */
bool
satisfy_recovery_target(const pgBackup *backup, const pgRecoveryTarget *rt)
{
	if (rt->xid_string)
		return backup->recovery_xid <= rt->target_xid;

	if (rt->time_string)
		return backup->recovery_time <= rt->target_time;

	if (rt->lsn_string)
		return backup->stop_lsn <= rt->target_lsn;

	return true;
}

/* TODO description */
bool
satisfy_timeline(const parray *timelines, const pgBackup *backup)
{
	int			i;

	for (i = 0; i < parray_num(timelines); i++)
	{
		TimeLineHistoryEntry *timeline;

		timeline = (TimeLineHistoryEntry *) parray_get(timelines, i);
		if (backup->tli == timeline->tli &&
			(XLogRecPtrIsInvalid(timeline->end) ||
			 backup->stop_lsn <= timeline->end))
			return true;
	}
	return false;
}

/* timelines represents a history of one particular timeline,
 * we must determine whether a target tli is part of that history.
 *
 *           /--------*
 * ---------*-------------->
 */
bool
tliIsPartOfHistory(const parray *timelines, TimeLineID tli)
{
	int			i;

	for (i = 0; i < parray_num(timelines); i++)
	{
		TimeLineHistoryEntry *timeline = (TimeLineHistoryEntry *) parray_get(timelines, i);

		if (tli == timeline->tli)
			return true;
	}

	return false;
}
/*
 * Get recovery options in the string format, parse them
 * and fill up the pgRecoveryTarget structure.
 */
pgRecoveryTarget *
parseRecoveryTargetOptions(const char *target_time,
					const char *target_xid,
					const char *target_inclusive,
					TimeLineID	target_tli,
					const char *target_lsn,
					const char *target_stop,
					const char *target_name,
					const char *target_action)
{
	bool		dummy_bool;
	/*
	 * count the number of the mutually exclusive options which may specify
	 * recovery target. If final value > 1, throw an error.
	 */
	int			recovery_target_specified = 0;
	pgRecoveryTarget *rt = pgut_new(pgRecoveryTarget);

	/* fill all options with default values */
	MemSet(rt, 0, sizeof(pgRecoveryTarget));

	/* parse given options */
	if (target_time)
	{
		time_t		dummy_time;

		recovery_target_specified++;
		rt->time_string = target_time;

		if (parse_time(target_time, &dummy_time, false))
			rt->target_time = dummy_time;
		else
			elog(ERROR, "Invalid value for '--recovery-target-time' option '%s'",
				 target_time);
	}

	if (target_xid)
	{
		TransactionId dummy_xid;

		recovery_target_specified++;
		rt->xid_string = target_xid;

#ifdef PGPRO_EE
		if (parse_uint64(target_xid, &dummy_xid, 0))
#else
		if (parse_uint32(target_xid, &dummy_xid, 0))
#endif
			rt->target_xid = dummy_xid;
		else
			elog(ERROR, "Invalid value for '--recovery-target-xid' option '%s'",
				 target_xid);
	}

	if (target_lsn)
	{
		XLogRecPtr	dummy_lsn;

		recovery_target_specified++;
		rt->lsn_string = target_lsn;
		if (parse_lsn(target_lsn, &dummy_lsn))
			rt->target_lsn = dummy_lsn;
		else
			elog(ERROR, "Invalid value of '--recovery-target-lsn' option '%s'",
				 target_lsn);
	}

	if (target_inclusive)
	{
		rt->inclusive_specified = true;
		if (parse_bool(target_inclusive, &dummy_bool))
			rt->target_inclusive = dummy_bool;
		else
			elog(ERROR, "Invalid value for '--recovery-target-inclusive' option '%s'",
				 target_inclusive);
	}

	rt->target_tli = target_tli;
	if (target_stop)
	{
		if ((strcmp(target_stop, "immediate") != 0)
			&& (strcmp(target_stop, "latest") != 0))
			elog(ERROR, "Invalid value for '--recovery-target' option '%s'",
				 target_stop);

		recovery_target_specified++;
		rt->target_stop = target_stop;
	}

	if (target_name)
	{
		recovery_target_specified++;
		rt->target_name = target_name;
	}

	if (target_action)
	{
		if ((strcmp(target_action, "pause") != 0)
			&& (strcmp(target_action, "promote") != 0)
			&& (strcmp(target_action, "shutdown") != 0))
			elog(ERROR, "Invalid value for '--recovery-target-action' option '%s'",
				 target_action);

		rt->target_action = target_action;
	}

	/* More than one mutually exclusive option was defined. */
	if (recovery_target_specified > 1)
		elog(ERROR, "At most one of '--recovery-target', '--recovery-target-name', "
					"'--recovery-target-time', '--recovery-target-xid' or "
					"'--recovery-target-lsn' options can be specified");

	/*
	 * If none of the options is defined, '--recovery-target-inclusive' option
	 * is meaningless.
	 */
	if (!(rt->xid_string || rt->time_string || rt->lsn_string) &&
		rt->target_inclusive)
		elog(ERROR, "The '--recovery-target-inclusive' option can be applied only when "
					"either of '--recovery-target-time', '--recovery-target-xid' or "
					"'--recovery-target-lsn' options is specified");

	/* If none of the options is defined, '--recovery-target-action' is meaningless */
	if (rt->target_action && recovery_target_specified == 0)
		elog(ERROR, "The '--recovery-target-action' option can be applied only when "
			"either of '--recovery-target', '--recovery-target-time', '--recovery-target-xid', "
			"'--recovery-target-lsn' or '--recovery-target-name' options is specified");

	/* TODO: sanity for recovery-target-timeline */

	return rt;
}

/*
 * Return array of dbOids of databases that should not be restored
 * Regardless of what option user used, db-include or db-exclude,
 * we always convert it into exclude_list.
 */
parray *
get_dbOid_exclude_list(pgBackup *backup, parray *datname_list,
										PartialRestoreType partial_restore_type)
{
	int i;
	int j;
//	pg_crc32	crc;
	parray		*database_map = NULL;
	parray		*dbOid_exclude_list = NULL;
	pgFile		*database_map_file = NULL;
	char		path[MAXPGPATH];
	char		database_map_path[MAXPGPATH];
	parray		*files = NULL;

	files = get_backup_filelist(backup, true);

	/* look for 'database_map' file in backup_content.control */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		if ((file->external_dir_num == 0) &&
			strcmp(DATABASE_MAP, file->name) == 0)
		{
			database_map_file = file;
			break;
		}
	}

	if (!database_map_file)
		elog(ERROR, "Backup %s doesn't contain a database_map, partial restore is impossible.",
			base36enc(backup->start_time));

	join_path_components(path, backup->root_dir, DATABASE_DIR);
	join_path_components(database_map_path, path, DATABASE_MAP);

	/* check database_map CRC */
//	crc = pgFileGetCRC(database_map_path, true, true, NULL, FIO_LOCAL_HOST);
//
//	if (crc != database_map_file->crc)
//		elog(ERROR, "Invalid CRC of backup file \"%s\" : %X. Expected %X",
//				database_map_file->path, crc, database_map_file->crc);

	/* get database_map from file */
	database_map = read_database_map(backup);

	/* partial restore requested but database_map is missing */
	if (!database_map)
		elog(ERROR, "Backup %s has empty or mangled database_map, partial restore is impossible.",
			base36enc(backup->start_time));

	/*
	 * So we have a list of datnames and a database_map for it.
	 * We must construct a list of dbOids to exclude.
	 */
	if (partial_restore_type == INCLUDE)
	{
		/* For 'include', keep dbOid of every datname NOT specified by user */
		for (i = 0; i < parray_num(datname_list); i++)
		{
			bool found_match = false;
			char   *datname = (char *) parray_get(datname_list, i);

			for (j = 0; j < parray_num(database_map); j++)
			{
				db_map_entry *db_entry = (db_map_entry *) parray_get(database_map, j);

				/* got a match */
				if (strcmp(db_entry->datname, datname) == 0)
				{
					found_match = true;
					/* for db-include we must exclude db_entry from database_map */
					parray_remove(database_map, j);
					j--;
				}
			}
			/* If specified datname is not found in database_map, error out */
			if (!found_match)
				elog(ERROR, "Failed to find a database '%s' in database_map of backup %s",
					datname, base36enc(backup->start_time));
		}

		/* At this moment only databases to exclude are left in the map */
		for (j = 0; j < parray_num(database_map); j++)
		{
			db_map_entry *db_entry = (db_map_entry *) parray_get(database_map, j);

			if (!dbOid_exclude_list)
				dbOid_exclude_list = parray_new();
			parray_append(dbOid_exclude_list, &db_entry->dbOid);
		}
	}
	else if (partial_restore_type == EXCLUDE)
	{
		/* For exclude, job is easier - find dbOid for every specified datname  */
		for (i = 0; i < parray_num(datname_list); i++)
		{
			bool found_match = false;
			char   *datname = (char *) parray_get(datname_list, i);

			for (j = 0; j < parray_num(database_map); j++)
			{
				db_map_entry *db_entry = (db_map_entry *) parray_get(database_map, j);

				/* got a match */
				if (strcmp(db_entry->datname, datname) == 0)
				{
					found_match = true;
					/* for db-exclude we must add dbOid to exclude list */
					if (!dbOid_exclude_list)
						dbOid_exclude_list = parray_new();
					parray_append(dbOid_exclude_list, &db_entry->dbOid);
				}
			}
			/* If specified datname is not found in database_map, error out */
			if (!found_match)
				elog(ERROR, "Failed to find a database '%s' in database_map of backup %s",
					datname, base36enc(backup->start_time));
		}
	}

	/* extra sanity: ensure that list is not empty */
	if (!dbOid_exclude_list || parray_num(dbOid_exclude_list) < 1)
		elog(ERROR, "Failed to find a match in database_map of backup %s for partial restore",
					base36enc(backup->start_time));

	/* clean backup filelist */
	if (files)
	{
		parray_walk(files, pgFileFree);
		parray_free(files);
	}

	/* sort dbOid array in ASC order */
	parray_qsort(dbOid_exclude_list, pgCompareOid);

	return dbOid_exclude_list;
}

/* Check that instance is suitable for incremental restore
 * Depending on type of incremental restore requirements are differs.
 *
 * TODO: add PG_CONTROL_IS_MISSING
 */
DestDirIncrCompatibility
check_incremental_compatibility(const char *pgdata, uint64 system_identifier,
								IncrRestoreMode incremental_mode)
{
	uint64	system_id_pgdata;
	bool    system_id_match = false;
	bool    success = true;
	bool    postmaster_is_up = false;
	bool    backup_label_exists = false;
	pid_t   pid;
	char    filename[MAXPGPATH];

	/* check postmaster pid */
	pid = fio_check_postmaster(pgdata, FIO_DB_HOST);

	if (pid == 1) /* postmaster.pid is mangled */
	{
		char pid_file[MAXPGPATH];

		join_path_components(pid_file, pgdata, "postmaster.pid");
		elog(WARNING, "Pid file \"%s\" is mangled, cannot determine whether postmaster is running or not",
			pid_file);
		success = false;
	}
	else if (pid > 1) /* postmaster is up */
	{
		elog(WARNING, "Postmaster with pid %u is running in destination directory \"%s\"",
			pid, pgdata);
		success = false;
		postmaster_is_up = true;
	}

	/* check that PG_VERSION is the same */

	/* slurp pg_control and check that system ID is the same
	 * check that instance is not running
	 * if lsn_based, check that there is no backup_label files is around AND
	 * get redo point lsn from destination pg_control.

	 * It is really important to be sure that pg_control is in cohesion with
	 * data files content, because based on pg_control information we will
	 * choose a backup suitable for lsn based incremental restore.
	 */
	elog(INFO, "Trying to read pg_control file in destination direstory");

	/* [Issue #313] check for previous failed incremental restore */
	join_path_components(filename, instance_config.pgdata, XLOG_CONTROL_BAK_FILE);
	if (fio_access(filename, F_OK, FIO_DB_HOST) == 0)
		system_id_pgdata = get_system_identifier(pgdata, XLOG_CONTROL_BAK_FILE, FIO_DB_HOST);
	else
		system_id_pgdata = get_system_identifier(pgdata, XLOG_CONTROL_FILE, FIO_DB_HOST);

	if (system_id_pgdata == instance_config.system_identifier)
		system_id_match = true;
	else
		elog(WARNING, "Backup catalog was initialized for system id %lu, "
					"but destination directory system id is %lu",
					system_identifier, system_id_pgdata);

	/*
	 * TODO: maybe there should be some other signs, pointing to pg_control
	 * desynchronization with cluster state.
	 */
	if (incremental_mode == INCR_LSN)
	{
		join_path_components(filename, pgdata, PG_BACKUP_LABEL_FILE);
		if (fio_access(filename, F_OK, FIO_DB_HOST) == 0)
		{
			elog(WARNING, "Destination directory contains \"backup_control\" file. "
				"This does NOT mean that you should delete this file and retry, only that "
				"incremental restore in 'lsn' mode may produce incorrect result, when applied "
				"to cluster with pg_control not synchronized with cluster state."
				"Consider to use incremental restore in 'checksum' mode");
			success = false;
			backup_label_exists = true;
		}
	}

	if (postmaster_is_up)
		return POSTMASTER_IS_RUNNING;

	/* PG_CONTROL MISSING */

	/* PG_CONTROL unreadable */

	if (!system_id_match)
		return SYSTEM_ID_MISMATCH;

	if (backup_label_exists)
		return BACKUP_LABEL_EXISTS;

	/* some other error condition */
	if (!success)
		return DEST_IS_NOT_OK;

	return DEST_OK;
}
