/*-------------------------------------------------------------------------
 *
 * backup.c: backup DB cluster, archived WAL
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#if PG_VERSION_NUM < 110000
#include "catalog/catalog.h"
#endif
#include "catalog/pg_tablespace.h"
#include "pgtar.h"
#include "streamutil.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "utils/thread.h"
#include "utils/file.h"

//const char *progname = "pg_probackup";

/* list of files contained in backup */
parray *backup_files_list = NULL;

/* We need critical section for datapagemap_add() in case of using threads */
static pthread_mutex_t backup_pagemap_mutex = PTHREAD_MUTEX_INITIALIZER;

// TODO: move to PGnodeInfo
bool exclusive_backup = false;

/* Is pg_start_backup() was executed */
bool backup_in_progress = false;

/*
 * Backup routines
 */
static void backup_cleanup(bool fatal, void *userdata);

static void *backup_files(void *arg);

static void do_backup_pg(InstanceState *instanceState, PGconn *backup_conn,
						 PGNodeInfo *nodeInfo, bool no_sync, bool backup_logs);

static void pg_switch_wal(PGconn *conn);

static void pg_stop_backup(InstanceState *instanceState, pgBackup *backup, PGconn *pg_startbackup_conn, PGNodeInfo *nodeInfo);

static void check_external_for_tablespaces(parray *external_list,
										   PGconn *backup_conn);
static parray *get_database_map(PGconn *pg_startbackup_conn);

/* pgpro specific functions */
static bool pgpro_support(PGconn *conn);

/* Check functions */
static bool pg_is_checksum_enabled(PGconn *conn);
static bool pg_is_in_recovery(PGconn *conn);
static bool pg_is_superuser(PGconn *conn);
static void check_server_version(PGconn *conn, PGNodeInfo *nodeInfo);
static void confirm_block_size(PGconn *conn, const char *name, int blcksz);
static void set_cfs_datafiles(parray *files, const char *root, char *relative, size_t i);

static StopBackupCallbackParams stop_callback_params;

static void
backup_stopbackup_callback(bool fatal, void *userdata)
{
	StopBackupCallbackParams *st = (StopBackupCallbackParams *) userdata;
	/*
	 * If backup is in progress, notify stop of backup to PostgreSQL
	 */
	if (backup_in_progress)
	{
		elog(WARNING, "backup in progress, stop backup");
		/* don't care about stop_lsn in case of error */
		pg_stop_backup_send(st->conn, st->server_version, current.from_replica, exclusive_backup, NULL);
	}
}

/*
 * Take a backup of a single postgresql instance.
 * Move files from 'pgdata' to a subdirectory in backup catalog.
 */
static void
do_backup_pg(InstanceState *instanceState, PGconn *backup_conn,
			 PGNodeInfo *nodeInfo, bool no_sync, bool backup_logs)
{
	int			i;
	char		external_prefix[MAXPGPATH]; /* Temp value. Used as template */
	char		dst_backup_path[MAXPGPATH];
	char		label[1024];
	XLogRecPtr	prev_backup_start_lsn = InvalidXLogRecPtr;

	/* arrays with meta info for multi threaded backup */
	pthread_t	*threads;
	backup_files_arg *threads_args;
	bool		backup_isok = true;

	pgBackup   *prev_backup = NULL;
	parray	   *prev_backup_filelist = NULL;
	parray	   *backup_list = NULL;
	parray	   *external_dirs = NULL;
	parray	   *database_map = NULL;

	/* used for multitimeline incremental backup */
	parray       *tli_list = NULL;

	/* for fancy reporting */
	time_t		start_time, end_time;
	char		pretty_time[20];
	char		pretty_bytes[20];

	elog(LOG, "Database backup start");
	if(current.external_dir_str)
	{
		external_dirs = make_external_directory_list(current.external_dir_str,
													 false);
		check_external_for_tablespaces(external_dirs, backup_conn);
	}

	/* notify start of backup to PostgreSQL server */
	time2iso(label, lengthof(label), current.start_time, false);
	strncat(label, " with pg_probackup", lengthof(label) -
			strlen(" with pg_probackup"));

	/* Call pg_start_backup function in PostgreSQL connect */
	pg_start_backup(label, smooth_checkpoint, &current, nodeInfo, backup_conn);

	/* Obtain current timeline */
#if PG_VERSION_NUM >= 90600
	current.tli = get_current_timeline(backup_conn);
#else
	current.tli = get_current_timeline_from_control(false);
#endif

	/*
	 * In incremental backup mode ensure that already-validated
	 * backup on current timeline exists and get its filelist.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE ||
		current.backup_mode == BACKUP_MODE_DIFF_PTRACK ||
		current.backup_mode == BACKUP_MODE_DIFF_DELTA)
	{
		/* get list of backups already taken */
		backup_list = catalog_get_backup_list(instanceState, INVALID_BACKUP_ID);

		prev_backup = catalog_get_last_data_backup(backup_list, current.tli, current.start_time);
		if (prev_backup == NULL)
		{
			/* try to setup multi-timeline backup chain */
			elog(WARNING, "Valid full backup on current timeline %u is not found, "
						"trying to look up on previous timelines",
						current.tli);

			tli_list = get_history_streaming(&instance_config.conn_opt, current.tli, backup_list);
			if (!tli_list)
			{
				elog(WARNING, "Failed to obtain current timeline history file via replication protocol");
				/* fallback to using archive */
				tli_list = catalog_get_timelines(instanceState, &instance_config);
			}

			if (parray_num(tli_list) == 0)
				elog(WARNING, "Cannot find valid backup on previous timelines, "
							"WAL archive is not available");
			else
			{
				prev_backup = get_multi_timeline_parent(backup_list, tli_list, current.tli,
														current.start_time, &instance_config);

				if (prev_backup == NULL)
					elog(WARNING, "Cannot find valid backup on previous timelines");
			}

			/* failed to find suitable parent, error out */
			if (!prev_backup)
				elog(ERROR, "Create new full backup before an incremental one");
		}
	}

	if (prev_backup)
	{
		if (parse_program_version(prev_backup->program_version) > parse_program_version(PROGRAM_VERSION))
			elog(ERROR, "pg_probackup binary version is %s, but backup %s version is %s. "
						"pg_probackup do not guarantee to be forward compatible. "
						"Please upgrade pg_probackup binary.",
						PROGRAM_VERSION, base36enc(prev_backup->start_time), prev_backup->program_version);

		elog(INFO, "Parent backup: %s", base36enc(prev_backup->start_time));

		/* Files of previous backup needed by DELTA backup */
		prev_backup_filelist = get_backup_filelist(prev_backup, true);

		/* If lsn is not NULL, only pages with higher lsn will be copied. */
		prev_backup_start_lsn = prev_backup->start_lsn;
		current.parent_backup = prev_backup->start_time;

		write_backup(&current, true);
	}

	/*
	 * It`s illegal to take PTRACK backup if LSN from ptrack_control() is not
	 * equal to start_lsn of previous backup.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		XLogRecPtr	ptrack_lsn = get_last_ptrack_lsn(backup_conn, nodeInfo);

		// new ptrack (>=2.0) is more robust and checks Start LSN
		if (ptrack_lsn > prev_backup->start_lsn || ptrack_lsn == InvalidXLogRecPtr)
		{
			elog(ERROR, "LSN from ptrack_control %X/%X is greater than Start LSN of previous backup %X/%X.\n"
						"Create new full backup before an incremental one.",
						(uint32) (ptrack_lsn >> 32), (uint32) (ptrack_lsn),
						(uint32) (prev_backup->start_lsn >> 32),
						(uint32) (prev_backup->start_lsn));
		}
	}

	/* For incremental backup check that start_lsn is not from the past
	 * Though it will not save us if PostgreSQL instance is actually
	 * restored STREAM backup.
	 */
	if (current.backup_mode != BACKUP_MODE_FULL &&
		prev_backup->start_lsn > current.start_lsn)
			elog(ERROR, "Current START LSN %X/%X is lower than START LSN %X/%X of previous backup %s. "
				"It may indicate that we are trying to backup PostgreSQL instance from the past.",
				(uint32) (current.start_lsn >> 32), (uint32) (current.start_lsn),
				(uint32) (prev_backup->start_lsn >> 32), (uint32) (prev_backup->start_lsn),
				base36enc(prev_backup->start_time));

	/* Update running backup meta with START LSN */
	write_backup(&current, true);

	/* In PAGE mode or in ARCHIVE wal-mode wait for current segment */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE || !current.stream)
	{
		/* Check that archive_dir can be reached */
		if (fio_access(instanceState->instance_wal_subdir_path, F_OK, FIO_BACKUP_HOST) != 0)
			elog(ERROR, "WAL archive directory is not accessible \"%s\": %s",
				instanceState->instance_wal_subdir_path, strerror(errno));

		/*
		 * Do not wait start_lsn for stream backup.
		 * Because WAL streaming will start after pg_start_backup() in stream
		 * mode.
		 */
		wait_wal_lsn(instanceState->instance_wal_subdir_path, current.start_lsn, true, current.tli, false, true, ERROR, false);
	}

	/* start stream replication */
	if (current.stream)
	{
		join_path_components(dst_backup_path, current.database_dir, PG_XLOG_DIR);
		fio_mkdir(dst_backup_path, DIR_PERMISSION, FIO_BACKUP_HOST);

		start_WAL_streaming(backup_conn, dst_backup_path, &instance_config.conn_opt,
							current.start_lsn, current.tli);

		/* Make sure that WAL streaming is working
		 * PAGE backup in stream mode is waited twice, first for
		 * segment in WAL archive and then for streamed segment
		 */
		wait_wal_lsn(dst_backup_path, current.start_lsn, true, current.tli, false, true, ERROR, true);
	}

	/* initialize backup's file list */
	backup_files_list = parray_new();
	join_path_components(external_prefix, current.root_dir, EXTERNAL_DIR);

	/* list files with the logical path. omit $PGDATA */
	fio_list_dir(backup_files_list, instance_config.pgdata,
				 true, true, false, backup_logs, true, 0);

	/*
	 * Get database_map (name to oid) for use in partial restore feature.
	 * It's possible that we fail and database_map will be NULL.
	 */
	database_map = get_database_map(backup_conn);

	/*
	 * Append to backup list all files and directories
	 * from external directory option
	 */
	if (external_dirs)
	{
		for (i = 0; i < parray_num(external_dirs); i++)
		{
			/* External dirs numeration starts with 1.
			 * 0 value is not external dir */
			if (fio_is_remote(FIO_DB_HOST))
				fio_list_dir(backup_files_list, parray_get(external_dirs, i),
							 false, true, false, false, true, i+1);
			else
				dir_list_file(backup_files_list, parray_get(external_dirs, i),
							  false, true, false, false, true, i+1, FIO_LOCAL_HOST);
		}
	}

	/* close ssh session in main thread */
	fio_disconnect();

	/* Sanity check for backup_files_list, thank you, Windows:
	 * https://github.com/postgrespro/pg_probackup/issues/48
	 */

	if (parray_num(backup_files_list) < 100)
		elog(ERROR, "PGDATA is almost empty. Either it was concurrently deleted or "
			"pg_probackup do not possess sufficient permissions to list PGDATA content");

	/* Calculate pgdata_bytes */
	for (i = 0; i < parray_num(backup_files_list); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(backup_files_list, i);

		if (file->external_dir_num != 0)
			continue;

		if (S_ISDIR(file->mode))
		{
			current.pgdata_bytes += 4096;
			continue;
		}

		current.pgdata_bytes += file->size;
	}

	pretty_size(current.pgdata_bytes, pretty_bytes, lengthof(pretty_bytes));
	elog(INFO, "PGDATA size: %s", pretty_bytes);

	/*
	 * Sort pathname ascending. It is necessary to create intermediate
	 * directories sequentially.
	 *
	 * For example:
	 * 1 - create 'base'
	 * 2 - create 'base/1'
	 *
	 * Sorted array is used at least in parse_filelist_filenames(),
	 * extractPageMap(), make_pagemap_from_ptrack().
	 */
	parray_qsort(backup_files_list, pgFileCompareRelPathWithExternal);

	/* Extract information about files in backup_list parsing their names:*/
	parse_filelist_filenames(backup_files_list, instance_config.pgdata);

	elog(LOG, "Current Start LSN: %X/%X, TLI: %X",
			(uint32) (current.start_lsn >> 32), (uint32) (current.start_lsn),
			current.tli);
	if (current.backup_mode != BACKUP_MODE_FULL)
		elog(LOG, "Parent Start LSN: %X/%X, TLI: %X",
			 (uint32) (prev_backup->start_lsn >> 32), (uint32) (prev_backup->start_lsn),
			 prev_backup->tli);

	/*
	 * Build page mapping in incremental mode.
	 */

	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE ||
		current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		bool pagemap_isok = true;

		time(&start_time);
		elog(INFO, "Extracting pagemap of changed blocks");

		if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
		{
			/*
			 * Build the page map. Obtain information about changed pages
			 * reading WAL segments present in archives up to the point
			 * where this backup has started.
			 */
			pagemap_isok = extractPageMap(instanceState->instance_wal_subdir_path,
						   instance_config.xlog_seg_size,
						   prev_backup->start_lsn, prev_backup->tli,
						   current.start_lsn, current.tli, tli_list);
		}
		else if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
		{
			/*
			 * Build the page map from ptrack information.
			 */
			make_pagemap_from_ptrack_2(backup_files_list, backup_conn,
									   nodeInfo->ptrack_schema,
									   nodeInfo->ptrack_version_num,
									   prev_backup_start_lsn);
		}

		time(&end_time);

		/* TODO: add ms precision */
		if (pagemap_isok)
			elog(INFO, "Pagemap successfully extracted, time elapsed: %.0f sec",
				 difftime(end_time, start_time));
		else
			elog(ERROR, "Pagemap extraction failed, time elasped: %.0f sec",
				 difftime(end_time, start_time));
	}

	/*
	 * Make directories before backup
	 */
	for (i = 0; i < parray_num(backup_files_list); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(backup_files_list, i);

		/* if the entry was a directory, create it in the backup */
		if (S_ISDIR(file->mode))
		{
			char		dirpath[MAXPGPATH];

			if (file->external_dir_num)
			{
				char		temp[MAXPGPATH];
				snprintf(temp, MAXPGPATH, "%s%d", external_prefix,
						 file->external_dir_num);
				join_path_components(dirpath, temp, file->rel_path);
			}
			else
				join_path_components(dirpath, current.database_dir, file->rel_path);

			elog(VERBOSE, "Create directory '%s'", dirpath);
			fio_mkdir(dirpath, DIR_PERMISSION, FIO_BACKUP_HOST);
		}

	}

	/* setup thread locks */
	pfilearray_clear_locks(backup_files_list);

	/* Sort by size for load balancing */
	parray_qsort(backup_files_list, pgFileCompareSize);
	/* Sort the array for binary search */
	if (prev_backup_filelist)
		parray_qsort(prev_backup_filelist, pgFileCompareRelPathWithExternal);

	/* write initial backup_content.control file and update backup.control  */
	write_backup_filelist(&current, backup_files_list,
						  instance_config.pgdata, external_dirs, true);
	write_backup(&current, true);

	/* Init backup page header map */
	init_header_map(&current);

	/* init thread args with own file lists */
	threads = (pthread_t *) palloc(sizeof(pthread_t) * num_threads);
	threads_args = (backup_files_arg *) palloc(sizeof(backup_files_arg)*num_threads);

	for (i = 0; i < num_threads; i++)
	{
		backup_files_arg *arg = &(threads_args[i]);

		arg->nodeInfo = nodeInfo;
		arg->from_root = instance_config.pgdata;
		arg->to_root = current.database_dir;
		arg->external_prefix = external_prefix;
		arg->external_dirs = external_dirs;
		arg->files_list = backup_files_list;
		arg->prev_filelist = prev_backup_filelist;
		arg->prev_start_lsn = prev_backup_start_lsn;
		arg->hdr_map = &(current.hdr_map);
		arg->thread_num = i+1;
		/* By default there are some error */
		arg->ret = 1;
	}

	/* Run threads */
	thread_interrupted = false;
	elog(INFO, "Start transferring data files");
	time(&start_time);
	for (i = 0; i < num_threads; i++)
	{
		backup_files_arg *arg = &(threads_args[i]);

		elog(VERBOSE, "Start thread num: %i", i);
		pthread_create(&threads[i], NULL, backup_files, arg);
	}

	/* Wait threads */
	for (i = 0; i < num_threads; i++)
	{
		pthread_join(threads[i], NULL);
		if (threads_args[i].ret == 1)
			backup_isok = false;
	}

	time(&end_time);
	pretty_time_interval(difftime(end_time, start_time),
						 pretty_time, lengthof(pretty_time));
	if (backup_isok)
		elog(INFO, "Data files are transferred, time elapsed: %s",
			pretty_time);
	else
		elog(ERROR, "Data files transferring failed, time elapsed: %s",
			pretty_time);

	/* clean previous backup file list */
	if (prev_backup_filelist)
	{
		parray_walk(prev_backup_filelist, pgFileFree);
		parray_free(prev_backup_filelist);
	}

	/* Notify end of backup */
	pg_stop_backup(instanceState, &current, backup_conn, nodeInfo);

	/* In case of backup from replica >= 9.6 we must fix minRecPoint,
	 * First we must find pg_control in backup_files_list.
	 */
	if (current.from_replica && !exclusive_backup)
	{
		pgFile	   *pg_control = NULL;

		for (i = 0; i < parray_num(backup_files_list); i++)
		{
			pgFile	   *tmp_file = (pgFile *) parray_get(backup_files_list, i);

			if (tmp_file->external_dir_num == 0 &&
				(strcmp(tmp_file->rel_path, XLOG_CONTROL_FILE) == 0))
			{
				pg_control = tmp_file;
				break;
			}
		}

		if (!pg_control)
			elog(ERROR, "Failed to find file \"%s\" in backup filelist.",
							XLOG_CONTROL_FILE);

		set_min_recovery_point(pg_control, current.database_dir, current.stop_lsn);
	}

	/* close and sync page header map */
	if (current.hdr_map.fp)
	{
		cleanup_header_map(&(current.hdr_map));

		if (fio_sync(current.hdr_map.path, FIO_BACKUP_HOST) != 0)
			elog(ERROR, "Cannot sync file \"%s\": %s", current.hdr_map.path, strerror(errno));
	}

	/* close ssh session in main thread */
	fio_disconnect();

	/*
	 * Add archived xlog files into the list of files of this backup
	 * NOTHING TO DO HERE
	 */

	/* write database map to file and add it to control file */
	if (database_map)
	{
		write_database_map(&current, database_map, backup_files_list);
		/* cleanup */
		parray_walk(database_map, db_map_entry_free);
		parray_free(database_map);
	}

	/* Print the list of files to backup catalog */
	write_backup_filelist(&current, backup_files_list, instance_config.pgdata,
						  external_dirs, true);
	/* update backup control file to update size info */
	write_backup(&current, true);

	/* Sync all copied files unless '--no-sync' flag is used */
	if (no_sync)
		elog(WARNING, "Backup files are not synced to disk");
	else
	{
		elog(INFO, "Syncing backup files to disk");
		time(&start_time);

		for (i = 0; i < parray_num(backup_files_list); i++)
		{
			char    to_fullpath[MAXPGPATH];
			pgFile *file = (pgFile *) parray_get(backup_files_list, i);

			/* TODO: sync directory ? */
			if (S_ISDIR(file->mode))
				continue;

			if (file->write_size <= 0)
				continue;

			/* construct fullpath */
			if (file->external_dir_num == 0)
				join_path_components(to_fullpath, current.database_dir, file->rel_path);
			else
			{
				char 	external_dst[MAXPGPATH];

				makeExternalDirPathByNum(external_dst, external_prefix,
										 file->external_dir_num);
				join_path_components(to_fullpath, external_dst, file->rel_path);
			}

			if (fio_sync(to_fullpath, FIO_BACKUP_HOST) != 0)
				elog(ERROR, "Cannot sync file \"%s\": %s", to_fullpath, strerror(errno));
		}

		time(&end_time);
		pretty_time_interval(difftime(end_time, start_time),
							 pretty_time, lengthof(pretty_time));
		elog(INFO, "Backup files are synced, time elapsed: %s", pretty_time);
	}

	/* be paranoid about instance been from the past */
	if (current.backup_mode != BACKUP_MODE_FULL &&
		current.stop_lsn < prev_backup->stop_lsn)
			elog(ERROR, "Current backup STOP LSN %X/%X is lower than STOP LSN %X/%X of previous backup %s. "
				"It may indicate that we are trying to backup PostgreSQL instance from the past.",
				(uint32) (current.stop_lsn >> 32), (uint32) (current.stop_lsn),
				(uint32) (prev_backup->stop_lsn >> 32), (uint32) (prev_backup->stop_lsn),
				base36enc(prev_backup->stop_lsn));

	/* clean external directories list */
	if (external_dirs)
		free_dir_list(external_dirs);

	/* Cleanup */
	if (backup_list)
	{
		parray_walk(backup_list, pgBackupFree);
		parray_free(backup_list);
	}

	if (tli_list)
	{
		parray_walk(tli_list, timelineInfoFree);
		parray_free(tli_list);
	}

	parray_walk(backup_files_list, pgFileFree);
	parray_free(backup_files_list);
	backup_files_list = NULL;
}

/*
 * Common code for CHECKDB and BACKUP commands.
 * Ensure that we're able to connect to the instance
 * check compatibility and fill basic info.
 * For checkdb launched in amcheck mode with pgdata validation
 * do not check system ID, it gives user an opportunity to
 * check remote PostgreSQL instance.
 * Also checking system ID in this case serves no purpose, because
 * all work is done by server.
 *
 * Returns established connection
 */
PGconn *
pgdata_basic_setup(ConnectionOptions conn_opt, PGNodeInfo *nodeInfo)
{
	PGconn *cur_conn;

	/* Create connection for PostgreSQL */
	cur_conn = pgut_connect(conn_opt.pghost, conn_opt.pgport,
							   conn_opt.pgdatabase,
							   conn_opt.pguser);

	current.primary_conninfo = pgut_get_conninfo_string(cur_conn);

	/* Confirm data block size and xlog block size are compatible */
	confirm_block_size(cur_conn, "block_size", BLCKSZ);
	confirm_block_size(cur_conn, "wal_block_size", XLOG_BLCKSZ);
	nodeInfo->block_size = BLCKSZ;
	nodeInfo->wal_block_size = XLOG_BLCKSZ;
	nodeInfo->is_superuser = pg_is_superuser(cur_conn);
	nodeInfo->pgpro_support = pgpro_support(cur_conn);

	current.from_replica = pg_is_in_recovery(cur_conn);

	/* Confirm that this server version is supported */
	check_server_version(cur_conn, nodeInfo);

	if (pg_is_checksum_enabled(cur_conn))
		current.checksum_version = 1;
	else
		current.checksum_version = 0;

	nodeInfo->checksum_version = current.checksum_version;

	if (current.checksum_version)
		elog(LOG, "This PostgreSQL instance was initialized with data block checksums. "
					"Data block corruption will be detected");
	else
		elog(WARNING, "This PostgreSQL instance was initialized without data block checksums. "
						"pg_probackup have no way to detect data block corruption without them. "
						"Reinitialize PGDATA with option '--data-checksums'.");

	if (nodeInfo->is_superuser)
		elog(WARNING, "Current PostgreSQL role is superuser. "
						"It is not recommended to run backup or checkdb as superuser.");

	strlcpy(current.server_version, nodeInfo->server_version_str,
			sizeof(current.server_version));

	return cur_conn;
}

/*
 * Entry point of pg_probackup BACKUP subcommand.
 */
int
do_backup(InstanceState *instanceState, pgSetBackupParams *set_backup_params,
		  bool no_validate, bool no_sync, bool backup_logs)
{
	PGconn		*backup_conn = NULL;
	PGNodeInfo	nodeInfo;
	char		pretty_bytes[20];

	/* Initialize PGInfonode */
	pgNodeInit(&nodeInfo);

	/* Save list of external directories */
	if (instance_config.external_dir_str &&
		(pg_strcasecmp(instance_config.external_dir_str, "none") != 0))
		current.external_dir_str = instance_config.external_dir_str;

	/* Create backup directory and BACKUP_CONTROL_FILE */
	pgBackupCreateDir(&current, instanceState->instance_backup_subdir_path);

	if (!instance_config.pgdata)
		elog(ERROR, "required parameter not specified: PGDATA "
						 "(-D, --pgdata)");

	/* Update backup status and other metainfo. */
	current.status = BACKUP_STATUS_RUNNING;
	current.start_time = current.backup_id;

	strlcpy(current.program_version, PROGRAM_VERSION,
			sizeof(current.program_version));

	current.compress_alg = instance_config.compress_alg;
	current.compress_level = instance_config.compress_level;

	elog(INFO, "Backup start, pg_probackup version: %s, instance: %s, backup ID: %s, backup mode: %s, "
			"wal mode: %s, remote: %s, compress-algorithm: %s, compress-level: %i",
			PROGRAM_VERSION, instanceState->instance_name, base36enc(current.backup_id), pgBackupGetBackupMode(&current, false),
			current.stream ? "STREAM" : "ARCHIVE", IsSshProtocol()  ? "true" : "false",
			deparse_compress_alg(current.compress_alg), current.compress_level);

	if (!lock_backup(&current, true, true))
		elog(ERROR, "Cannot lock backup %s directory",
			 base36enc(current.backup_id));
	write_backup(&current, true);

	/* set the error processing function for the backup process */
	pgut_atexit_push(backup_cleanup, NULL);

	elog(LOG, "Backup destination is initialized");

	/*
	 * setup backup_conn, do some compatibility checks and
	 * fill basic info about instance
	 */
	backup_conn = pgdata_basic_setup(instance_config.conn_opt, &nodeInfo);

	if (current.from_replica)
		elog(INFO, "Backup %s is going to be taken from standby", base36enc(current.backup_id));

	/* TODO, print PostgreSQL full version */
	//elog(INFO, "PostgreSQL version: %s", nodeInfo.server_version_str);

	/*
	 * Ensure that backup directory was initialized for the same PostgreSQL
	 * instance we opened connection to. And that target backup database PGDATA
	 * belogns to the same instance.
	 */
	check_system_identifiers(backup_conn, instance_config.pgdata);

	/* below perform checks specific for backup command */
#if PG_VERSION_NUM >= 110000
	if (!RetrieveWalSegSize(backup_conn))
		elog(ERROR, "Failed to retrieve wal_segment_size");
#endif

	get_ptrack_version(backup_conn, &nodeInfo);
	//	elog(WARNING, "ptrack_version_num %d", ptrack_version_num);

	if (nodeInfo.ptrack_version_num > 0)
		nodeInfo.is_ptrack_enable = pg_ptrack_enable(backup_conn, nodeInfo.ptrack_version_num);

	if (current.backup_mode == BACKUP_MODE_DIFF_PTRACK)
	{
		/* ptrack_version_num < 2.0 was already checked in get_ptrack_version() */
		if (nodeInfo.ptrack_version_num == 0)
			elog(ERROR, "This PostgreSQL instance does not support ptrack");
		else
		{
			if (!nodeInfo.is_ptrack_enable)
				elog(ERROR, "Ptrack is disabled");
		}
	}

	if (current.from_replica && exclusive_backup)
		/* Check master connection options */
		if (instance_config.master_conn_opt.pghost == NULL)
			elog(ERROR, "Options for connection to master must be provided to perform backup from replica");

	/* add note to backup if requested */
	if (set_backup_params && set_backup_params->note)
		add_note(&current, set_backup_params->note);

	/* backup data */
	do_backup_pg(instanceState, backup_conn, &nodeInfo, no_sync, backup_logs);
	pgut_atexit_pop(backup_cleanup, NULL);

	/* compute size of wal files of this backup stored in the archive */
	if (!current.stream)
	{
		XLogSegNo start_segno;
		XLogSegNo stop_segno;

		GetXLogSegNo(current.start_lsn, start_segno, instance_config.xlog_seg_size);
		GetXLogSegNo(current.stop_lsn, stop_segno, instance_config.xlog_seg_size);
		current.wal_bytes = (stop_segno - start_segno) * instance_config.xlog_seg_size;

		/*
		 * If start_lsn and stop_lsn are located in the same segment, then
		 * set wal_bytes to the size of 1 segment.
		 */
		if (current.wal_bytes <= 0)
			current.wal_bytes = instance_config.xlog_seg_size;
	}

	/* Backup is done. Update backup status */
	current.end_time = time(NULL);
	current.status = BACKUP_STATUS_DONE;
	write_backup(&current, true);

	/* Pin backup if requested */
	if (set_backup_params &&
		(set_backup_params->ttl > 0 ||
		 set_backup_params->expire_time > 0))
	{
		pin_backup(&current, set_backup_params);
	}

	if (!no_validate)
		pgBackupValidate(&current, NULL);

	/* Notify user about backup size */
	if (current.stream)
		pretty_size(current.data_bytes + current.wal_bytes, pretty_bytes, lengthof(pretty_bytes));
	else
		pretty_size(current.data_bytes, pretty_bytes, lengthof(pretty_bytes));
	elog(INFO, "Backup %s resident size: %s", base36enc(current.start_time), pretty_bytes);

	if (current.status == BACKUP_STATUS_OK ||
		current.status == BACKUP_STATUS_DONE)
		elog(INFO, "Backup %s completed", base36enc(current.start_time));
	else
		elog(ERROR, "Backup %s failed", base36enc(current.start_time));

	/*
	 * After successful backup completion remove backups
	 * which are expired according to retention policies
	 */
	if (delete_expired || merge_expired || delete_wal)
		do_retention(instanceState, no_validate, no_sync);

	return 0;
}

/*
 * Confirm that this server version is supported
 */
static void
check_server_version(PGconn *conn, PGNodeInfo *nodeInfo)
{
	PGresult   *res = NULL;

	/* confirm server version */
	nodeInfo->server_version = PQserverVersion(conn);

	if (nodeInfo->server_version == 0)
		elog(ERROR, "Unknown server version %d", nodeInfo->server_version);

	if (nodeInfo->server_version < 100000)
		sprintf(nodeInfo->server_version_str, "%d.%d",
				nodeInfo->server_version / 10000,
				(nodeInfo->server_version / 100) % 100);
	else
		sprintf(nodeInfo->server_version_str, "%d",
				nodeInfo->server_version / 10000);

	if (nodeInfo->server_version < 90500)
		elog(ERROR,
			 "server version is %s, must be %s or higher",
			 nodeInfo->server_version_str, "9.5");

	if (current.from_replica && nodeInfo->server_version < 90600)
		elog(ERROR,
			 "server version is %s, must be %s or higher for backup from replica",
			 nodeInfo->server_version_str, "9.6");

	if (nodeInfo->pgpro_support)
		res = pgut_execute(conn, "SELECT pgpro_edition()", 0, NULL);

	/*
	 * Check major version of connected PostgreSQL and major version of
	 * compiled PostgreSQL.
	 */
#ifdef PGPRO_VERSION
	if (!res)
		/* It seems we connected to PostgreSQL (not Postgres Pro) */
		elog(ERROR, "%s was built with Postgres Pro %s %s, "
					"but connection is made with PostgreSQL %s",
			 PROGRAM_NAME, PG_MAJORVERSION, PGPRO_EDITION, nodeInfo->server_version_str);
	else
	{
		if (strcmp(nodeInfo->server_version_str, PG_MAJORVERSION) != 0 &&
				 strcmp(PQgetvalue(res, 0, 0), PGPRO_EDITION) != 0)
			elog(ERROR, "%s was built with Postgres Pro %s %s, "
						"but connection is made with Postgres Pro %s %s",
				 PROGRAM_NAME, PG_MAJORVERSION, PGPRO_EDITION,
				 nodeInfo->server_version_str, PQgetvalue(res, 0, 0));
	}
#else
	if (res)
		/* It seems we connected to Postgres Pro (not PostgreSQL) */
		elog(ERROR, "%s was built with PostgreSQL %s, "
					"but connection is made with Postgres Pro %s %s",
			 PROGRAM_NAME, PG_MAJORVERSION,
			 nodeInfo->server_version_str, PQgetvalue(res, 0, 0));
	else
	{
		if (strcmp(nodeInfo->server_version_str, PG_MAJORVERSION) != 0)
			elog(ERROR, "%s was built with PostgreSQL %s, but connection is made with %s",
				 PROGRAM_NAME, PG_MAJORVERSION, nodeInfo->server_version_str);
	}
#endif

	if (res)
		PQclear(res);

	/* Do exclusive backup only for PostgreSQL 9.5 */
	exclusive_backup = nodeInfo->server_version < 90600;
}

/*
 * Ensure that backup directory was initialized for the same PostgreSQL
 * instance we opened connection to. And that target backup database PGDATA
 * belogns to the same instance.
 * All system identifiers must be equal.
 */
void
check_system_identifiers(PGconn *conn, char *pgdata)
{
	uint64		system_id_conn;
	uint64		system_id_pgdata;

	system_id_pgdata = get_system_identifier(pgdata, XLOG_CONTROL_FILE, FIO_DB_HOST);
	system_id_conn = get_remote_system_identifier(conn);

	/* for checkdb check only system_id_pgdata and system_id_conn */
	if (current.backup_mode == BACKUP_MODE_INVALID)
	{
		if (system_id_conn != system_id_pgdata)
		{
			elog(ERROR, "Data directory initialized with system id " UINT64_FORMAT ", "
						"but connected instance system id is " UINT64_FORMAT,
				 system_id_pgdata, system_id_conn);
		}
		return;
	}

	if (system_id_conn != instance_config.system_identifier)
		elog(ERROR, "Backup data directory was initialized for system id " UINT64_FORMAT ", "
					"but connected instance system id is " UINT64_FORMAT,
			 instance_config.system_identifier, system_id_conn);

	if (system_id_pgdata != instance_config.system_identifier)
		elog(ERROR, "Backup data directory was initialized for system id " UINT64_FORMAT ", "
					"but target backup directory system id is " UINT64_FORMAT,
			 instance_config.system_identifier, system_id_pgdata);
}

/*
 * Ensure that target backup database is initialized with
 * compatible settings. Currently check BLCKSZ and XLOG_BLCKSZ.
 */
static void
confirm_block_size(PGconn *conn, const char *name, int blcksz)
{
	PGresult   *res;
	char	   *endp;
	int			block_size;

	res = pgut_execute(conn, "SELECT pg_catalog.current_setting($1)", 1, &name);
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
		elog(ERROR, "cannot get %s: %s", name, PQerrorMessage(conn));

	block_size = strtol(PQgetvalue(res, 0, 0), &endp, 10);
	if ((endp && *endp) || block_size != blcksz)
		elog(ERROR,
			 "%s(%d) is not compatible(%d expected)",
			 name, block_size, blcksz);

	PQclear(res);
}

/*
 * Notify start of backup to PostgreSQL server.
 */
void
pg_start_backup(const char *label, bool smooth, pgBackup *backup,
				PGNodeInfo *nodeInfo, PGconn *conn)
{
	PGresult   *res;
	const char *params[2];
	uint32		lsn_hi;
	uint32		lsn_lo;
	params[0] = label;

	elog(INFO, "wait for pg_start_backup()");

	/* 2nd argument is 'fast'*/
	params[1] = smooth ? "false" : "true";
	if (!exclusive_backup)
		res = pgut_execute(conn,
						   "SELECT pg_catalog.pg_start_backup($1, $2, false)",
						   2,
						   params);
	else
		res = pgut_execute(conn,
						   "SELECT pg_catalog.pg_start_backup($1, $2)",
						   2,
						   params);

	/*
	 * Set flag that pg_start_backup() was called. If an error will happen it
	 * is necessary to call pg_stop_backup() in backup_cleanup().
	 */
	backup_in_progress = true;
	stop_callback_params.conn = conn;
	stop_callback_params.server_version = nodeInfo->server_version;
	pgut_atexit_push(backup_stopbackup_callback, &stop_callback_params);

	/* Extract timeline and LSN from results of pg_start_backup() */
	XLogDataFromLSN(PQgetvalue(res, 0, 0), &lsn_hi, &lsn_lo);
	/* Calculate LSN */
	backup->start_lsn = ((uint64) lsn_hi )<< 32 | lsn_lo;

	PQclear(res);

	if ((!backup->stream || backup->backup_mode == BACKUP_MODE_DIFF_PAGE) &&
		!backup->from_replica &&
		!(nodeInfo->server_version < 90600 &&
		  !nodeInfo->is_superuser))
		/*
		 * Switch to a new WAL segment. It is necessary to get archived WAL
		 * segment, which includes start LSN of current backup.
		 * Don`t do this for replica backups and for PG 9.5 if pguser is not superuser
		 * (because in 9.5 only superuser can switch WAL)
		 */
		pg_switch_wal(conn);
}

/*
 * Switch to a new WAL segment. It should be called only for master.
 * For PG 9.5 it should be called only if pguser is superuser.
 */
static void
pg_switch_wal(PGconn *conn)
{
	PGresult   *res;

	pg_silent_client_messages(conn);

#if PG_VERSION_NUM >= 100000
	res = pgut_execute(conn, "SELECT pg_catalog.pg_switch_wal()", 0, NULL);
#else
	res = pgut_execute(conn, "SELECT pg_catalog.pg_switch_xlog()", 0, NULL);
#endif

	PQclear(res);
}

/*
 * Check if the instance is PostgresPro fork.
 */
static bool
pgpro_support(PGconn *conn)
{
	PGresult   *res;

	res = pgut_execute(conn,
						  "SELECT proname FROM pg_proc WHERE proname='pgpro_edition'",
						  0, NULL);

	if (PQresultStatus(res) == PGRES_TUPLES_OK &&
		(PQntuples(res) == 1) &&
		(strcmp(PQgetvalue(res, 0, 0), "pgpro_edition") == 0))
	{
		PQclear(res);
		return true;
	}

	PQclear(res);
	return false;
}

/*
 * Fill 'datname to Oid' map
 *
 * This function can fail to get the map for legal reasons, e.g. missing
 * permissions on pg_database during `backup`.
 * As long as user do not use partial restore feature it`s fine.
 *
 * To avoid breaking a backward compatibility don't throw an ERROR,
 * throw a warning instead of an error and return NULL.
 * Caller is responsible for checking the result.
 */
parray *
get_database_map(PGconn *conn)
{
	PGresult   *res;
	parray *database_map = NULL;
	int i;

	/*
	 * Do not include template0 and template1 to the map
	 * as default databases that must always be restored.
	 */
	res = pgut_execute_extended(conn,
						  "SELECT oid, datname FROM pg_catalog.pg_database "
						  "WHERE datname NOT IN ('template1', 'template0')",
						  0, NULL, true, true);

	/* Don't error out, simply return NULL. See comment above. */
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		elog(WARNING, "Failed to get database map: %s",
			PQerrorMessage(conn));

		return NULL;
	}

	/* Construct database map */
	for (i = 0; i < PQntuples(res); i++)
	{
		char *datname = NULL;
		db_map_entry *db_entry = (db_map_entry *) pgut_malloc(sizeof(db_map_entry));

		/* get Oid */
		db_entry->dbOid = atoll(PQgetvalue(res, i, 0));

		/* get datname */
		datname = PQgetvalue(res, i, 1);
		db_entry->datname = pgut_malloc(strlen(datname) + 1);
		strcpy(db_entry->datname, datname);

		if (database_map == NULL)
			database_map = parray_new();

		parray_append(database_map, db_entry);
	}

	return database_map;
}

/* Check if ptrack is enabled in target instance */
static bool
pg_is_checksum_enabled(PGconn *conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(conn, "SHOW data_checksums", 0, NULL);

	if (strcmp(PQgetvalue(res_db, 0, 0), "on") == 0)
	{
		PQclear(res_db);
		return true;
	}
	PQclear(res_db);
	return false;
}

/* Check if target instance is replica */
static bool
pg_is_in_recovery(PGconn *conn)
{
	PGresult   *res_db;

	res_db = pgut_execute(conn, "SELECT pg_catalog.pg_is_in_recovery()", 0, NULL);

	if (PQgetvalue(res_db, 0, 0)[0] == 't')
	{
		PQclear(res_db);
		return true;
	}
	PQclear(res_db);
	return false;
}


/* Check if current PostgreSQL role is superuser */
static bool
pg_is_superuser(PGconn *conn)
{
	PGresult   *res;

	res = pgut_execute(conn, "SELECT pg_catalog.current_setting('is_superuser')", 0, NULL);

	if (strcmp(PQgetvalue(res, 0, 0), "on") == 0)
	{
		PQclear(res);
		return true;
	}
	PQclear(res);
	return false;
}

/*
 * Wait for target LSN or WAL segment, containing target LSN.
 *
 * Depending on value of flag in_stream_dir wait for target LSN to archived or
 * streamed in 'archive_dir' or 'pg_wal' directory.
 *
 * If flag 'is_start_lsn' is set then issue warning for first-time users.
 * If flag 'in_prev_segment' is set, look for LSN in previous segment,
 *  with EndRecPtr >= Target LSN. It should be used only for solving
 *  invalid XRecOff problem.
 * If flag 'segment_only' is set, then, instead of waiting for LSN, wait for segment,
 *  containing that LSN.
 * If flags 'in_prev_segment' and 'segment_only' are both set, then wait for
 *  previous segment.
 *
 * Flag 'in_stream_dir' determine whether we looking for WAL in 'pg_wal' directory or
 * in archive. Do note, that we cannot rely sorely on global variable 'stream_wal' (current.stream) because,
 * for example, PAGE backup must(!) look for start_lsn in archive regardless of wal_mode.
 *
 * 'timeout_elevel' determine the elevel for timeout elog message. If elevel lighter than
 * ERROR is used, then return InvalidXLogRecPtr. TODO: return something more concrete, for example 1.
 *
 * Returns target LSN if such is found, failing that returns LSN of record prior to target LSN.
 * Returns InvalidXLogRecPtr if 'segment_only' flag is used.
 */
XLogRecPtr
wait_wal_lsn(const char *wal_segment_dir, XLogRecPtr target_lsn, bool is_start_lsn, TimeLineID tli,
			 bool in_prev_segment, bool segment_only,
			 int timeout_elevel, bool in_stream_dir)
{
	XLogSegNo	targetSegNo;
	char		wal_segment_path[MAXPGPATH],
				wal_segment[MAXFNAMELEN];
	bool		file_exists = false;
	uint32		try_count = 0,
				timeout;
	char		*wal_delivery_str = in_stream_dir ? "streamed":"archived";

#ifdef HAVE_LIBZ
	char		gz_wal_segment_path[MAXPGPATH];
#endif

	/* Compute the name of the WAL file containing requested LSN */
	GetXLogSegNo(target_lsn, targetSegNo, instance_config.xlog_seg_size);
	if (in_prev_segment)
		targetSegNo--;
	GetXLogFileName(wal_segment, tli, targetSegNo,
					instance_config.xlog_seg_size);

	join_path_components(wal_segment_path, wal_segment_dir, wal_segment);
	/*
	 * In pg_start_backup we wait for 'target_lsn' in 'pg_wal' directory if it is
	 * stream and non-page backup. Page backup needs archived WAL files, so we
	 * wait for 'target_lsn' in archive 'wal' directory for page backups.
	 *
	 * In pg_stop_backup it depends only on stream_wal.
	 */

	/* TODO: remove this in 3.0 (it is a cludge against some old bug with archive_timeout) */
	if (instance_config.archive_timeout > 0)
		timeout = instance_config.archive_timeout;
	else
		timeout = ARCHIVE_TIMEOUT_DEFAULT;

	if (segment_only)
		elog(LOG, "Looking for segment: %s", wal_segment);
	else
		elog(LOG, "Looking for LSN %X/%X in segment: %s",
			 (uint32) (target_lsn >> 32), (uint32) target_lsn, wal_segment);

#ifdef HAVE_LIBZ
	snprintf(gz_wal_segment_path, sizeof(gz_wal_segment_path), "%s.gz",
			 wal_segment_path);
#endif

	/* Wait until target LSN is archived or streamed */
	while (true)
	{
		if (!file_exists)
		{
			file_exists = fileExists(wal_segment_path, FIO_BACKUP_HOST);

			/* Try to find compressed WAL file */
			if (!file_exists)
			{
#ifdef HAVE_LIBZ
				file_exists = fileExists(gz_wal_segment_path, FIO_BACKUP_HOST);
				if (file_exists)
					elog(LOG, "Found compressed WAL segment: %s", wal_segment_path);
#endif
			}
			else
				elog(LOG, "Found WAL segment: %s", wal_segment_path);
		}

		if (file_exists)
		{
			/* Do not check for target LSN */
			if (segment_only)
				return InvalidXLogRecPtr;

			/*
			 * A WAL segment found. Look for target LSN in it.
			 */
			if (!XRecOffIsNull(target_lsn) &&
				  wal_contains_lsn(wal_segment_dir, target_lsn, tli,
									instance_config.xlog_seg_size))
				/* Target LSN was found */
			{
				elog(LOG, "Found LSN: %X/%X", (uint32) (target_lsn >> 32), (uint32) target_lsn);
				return target_lsn;
			}

			/*
			 * If we failed to get target LSN in a reasonable time, try
			 * to get LSN of last valid record prior to the target LSN. But only
			 * in case of a backup from a replica.
			 * Note, that with NullXRecOff target_lsn we do not wait
			 * for 'timeout / 2' seconds before going for previous record,
			 * because such LSN cannot be delivered at all.
			 *
			 * There are two cases for this:
			 * 1. Replica returned readpoint LSN which just do not exists. We want to look
			 *  for previous record in the same(!) WAL segment which endpoint points to this LSN.
			 * 2. Replica returened endpoint LSN with NullXRecOff. We want to look
			 *  for previous record which endpoint points greater or equal LSN in previous WAL segment.
			 */
			if (current.from_replica &&
				(XRecOffIsNull(target_lsn) || try_count > timeout / 2))
			{
				XLogRecPtr	res;

				res = get_prior_record_lsn(wal_segment_dir, current.start_lsn, target_lsn, tli,
										in_prev_segment, instance_config.xlog_seg_size);

				if (!XLogRecPtrIsInvalid(res))
				{
					/* LSN of the prior record was found */
					elog(LOG, "Found prior LSN: %X/%X",
						 (uint32) (res >> 32), (uint32) res);
					return res;
				}
			}
		}

		sleep(1);
		if (interrupted || thread_interrupted)
			elog(ERROR, "Interrupted during waiting for WAL %s", in_stream_dir ? "streaming" : "archiving");
		try_count++;

		/* Inform user if WAL segment is absent in first attempt */
		if (try_count == 1)
		{
			if (segment_only)
				elog(INFO, "Wait for WAL segment %s to be %s",
					 wal_segment_path, wal_delivery_str);
			else
				elog(INFO, "Wait for LSN %X/%X in %s WAL segment %s",
					 (uint32) (target_lsn >> 32), (uint32) target_lsn,
					 wal_delivery_str, wal_segment_path);
		}

		if (!current.stream && is_start_lsn && try_count == 30)
			elog(WARNING, "By default pg_probackup assume WAL delivery method to be ARCHIVE. "
				 "If continuous archiving is not set up, use '--stream' option to make autonomous backup. "
				 "Otherwise check that continuous archiving works correctly.");

		if (timeout > 0 && try_count > timeout)
		{
			if (file_exists)
				elog(timeout_elevel, "WAL segment %s was %s, "
					 "but target LSN %X/%X could not be %s in %d seconds",
					 wal_segment, wal_delivery_str,
					 (uint32) (target_lsn >> 32), (uint32) target_lsn,
					 wal_delivery_str, timeout);
			/* If WAL segment doesn't exist or we wait for previous segment */
			else
				elog(timeout_elevel,
					 "WAL segment %s could not be %s in %d seconds",
					 wal_segment, wal_delivery_str, timeout);

			return InvalidXLogRecPtr;
		}
	}
}

/*
 * Check stop_lsn (returned from pg_stop_backup()) and update backup->stop_lsn
 */
void
wait_wal_and_calculate_stop_lsn(const char *xlog_path, XLogRecPtr stop_lsn, pgBackup *backup)
{
	bool	 stop_lsn_exists = false;

	/* It is ok for replica to return invalid STOP LSN
	 * UPD: Apparently it is ok even for a master.
	 */
	if (!XRecOffIsValid(stop_lsn))
	{
		XLogSegNo	segno = 0;
		XLogRecPtr	lsn_tmp = InvalidXLogRecPtr;

		/*
		 * Even though the value is invalid, it's expected postgres behaviour
		 * and we're trying to fix it below.
		 */
		elog(LOG, "Invalid offset in stop_lsn value %X/%X, trying to fix",
			 (uint32) (stop_lsn >> 32), (uint32) (stop_lsn));

		/*
		 * Note: even with gdb it is very hard to produce automated tests for
		 * contrecord + invalid LSN, so emulate it for manual testing.
		 */
		//lsn = lsn - XLOG_SEG_SIZE;
		//elog(WARNING, "New Invalid stop_backup_lsn value %X/%X",
		//	 (uint32) (stop_lsn >> 32), (uint32) (stop_lsn));

		GetXLogSegNo(stop_lsn, segno, instance_config.xlog_seg_size);

		/*
		 * Note, that there is no guarantee that corresponding WAL file even exists.
		 * Replica may return LSN from future and keep staying in present.
		 * Or it can return invalid LSN.
		 *
		 * That's bad, since we want to get real LSN to save it in backup label file
		 * and to use it in WAL validation.
		 *
		 * So we try to do the following:
		 * 1. Wait 'archive_timeout' seconds for segment containing stop_lsn and
		 *	  look for the first valid record in it.
		 * 	  It solves the problem of occasional invalid LSN on write-busy system.
		 * 2. Failing that, look for record in previous segment with endpoint
		 *	  equal or greater than stop_lsn. It may(!) solve the problem of invalid LSN
		 *	  on write-idle system. If that fails too, error out.
		 */

		/* stop_lsn is pointing to a 0 byte of xlog segment */
		if (stop_lsn % instance_config.xlog_seg_size == 0)
		{
			/* Wait for segment with current stop_lsn, it is ok for it to never arrive */
			wait_wal_lsn(xlog_path, stop_lsn, false, backup->tli,
						 false, true, WARNING, backup->stream);

			/* Get the first record in segment with current stop_lsn */
			lsn_tmp = get_first_record_lsn(xlog_path, segno, backup->tli,
									       instance_config.xlog_seg_size,
									       instance_config.archive_timeout);

			/* Check that returned LSN is valid and greater than stop_lsn */
			if (XLogRecPtrIsInvalid(lsn_tmp) ||
				!XRecOffIsValid(lsn_tmp) ||
				lsn_tmp < stop_lsn)
			{
				/* Backup from master should error out here */
				if (!backup->from_replica)
					elog(ERROR, "Failed to get next WAL record after %X/%X",
								(uint32) (stop_lsn >> 32),
								(uint32) (stop_lsn));

				/* No luck, falling back to looking up for previous record */
				elog(WARNING, "Failed to get next WAL record after %X/%X, "
							"looking for previous WAL record",
							(uint32) (stop_lsn >> 32),
							(uint32) (stop_lsn));

				/* Despite looking for previous record there is not guarantee of success
				 * because previous record can be the contrecord.
				 */
				lsn_tmp = wait_wal_lsn(xlog_path, stop_lsn, false, backup->tli,
									   true, false, ERROR, backup->stream);

				/* sanity */
				if (!XRecOffIsValid(lsn_tmp) || XLogRecPtrIsInvalid(lsn_tmp))
					elog(ERROR, "Failed to get WAL record prior to %X/%X",
								(uint32) (stop_lsn >> 32),
								(uint32) (stop_lsn));
			}
		}
		/* stop lsn is aligned to xlog block size, just find next lsn */
		else if (stop_lsn % XLOG_BLCKSZ == 0)
		{
			/* Wait for segment with current stop_lsn */
			wait_wal_lsn(xlog_path, stop_lsn, false, backup->tli,
						 false, true, ERROR, backup->stream);

			/* Get the next closest record in segment with current stop_lsn */
			lsn_tmp = get_next_record_lsn(xlog_path, segno, backup->tli,
									       instance_config.xlog_seg_size,
									       instance_config.archive_timeout,
									       stop_lsn);

			/* sanity */
			if (!XRecOffIsValid(lsn_tmp) || XLogRecPtrIsInvalid(lsn_tmp))
				elog(ERROR, "Failed to get WAL record next to %X/%X",
							(uint32) (stop_lsn >> 32),
							(uint32) (stop_lsn));
		}
		/* PostgreSQL returned something very illegal as STOP_LSN, error out */
		else
			elog(ERROR, "Invalid stop_backup_lsn value %X/%X",
				 (uint32) (stop_lsn >> 32), (uint32) (stop_lsn));

		/* Setting stop_backup_lsn will set stop point for streaming */
		stop_backup_lsn = lsn_tmp;
		stop_lsn_exists = true;
	}

	elog(LOG, "stop_lsn: %X/%X",
		(uint32) (stop_lsn >> 32), (uint32) (stop_lsn));

	/*
	 * Wait for stop_lsn to be archived or streamed.
	 * If replica returned valid STOP_LSN of not actually existing record,
	 * look for previous record with endpoint >= STOP_LSN.
	 */
	if (!stop_lsn_exists)
		stop_backup_lsn = wait_wal_lsn(xlog_path, stop_lsn, false, backup->tli,
									false, false, ERROR, backup->stream);

	backup->stop_lsn = stop_backup_lsn;
}

/* Remove annoying NOTICE messages generated by backend */
void
pg_silent_client_messages(PGconn *conn)
{
	PGresult   *res;
	res = pgut_execute(conn, "SET client_min_messages = warning;",
					   0, NULL);
	PQclear(res);
}

void
pg_create_restore_point(PGconn *conn, time_t backup_start_time)
{
	PGresult	*res;
	const char	*params[1];
	char		name[1024];

	snprintf(name, lengthof(name), "pg_probackup, backup_id %s",
				base36enc(backup_start_time));
	params[0] = name;

	res = pgut_execute(conn, "SELECT pg_catalog.pg_create_restore_point($1)",
					   1, params);
	PQclear(res);
}

void
pg_stop_backup_send(PGconn *conn, int server_version, bool is_started_on_replica, bool is_exclusive, char **query_text)
{
	static const char
		stop_exlusive_backup_query[] =
			/*
			 * Stop the non-exclusive backup. Besides stop_lsn it returns from
			 * pg_stop_backup(false) copy of the backup label and tablespace map
			 * so they can be written to disk by the caller.
			 * TODO, question: add NULLs as backup_label and tablespace_map?
			 */
			"SELECT"
			" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
			" current_timestamp(0)::timestamptz,"
			" pg_catalog.pg_stop_backup() as lsn",
		stop_backup_on_master_query[] =
			"SELECT"
			" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
			" current_timestamp(0)::timestamptz,"
			" lsn,"
			" labelfile,"
			" spcmapfile"
			" FROM pg_catalog.pg_stop_backup(false, false)",
		stop_backup_on_master_before10_query[] =
			"SELECT"
			" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
			" current_timestamp(0)::timestamptz,"
			" lsn,"
			" labelfile,"
			" spcmapfile"
			" FROM pg_catalog.pg_stop_backup(false)",
		/*
		 * In case of backup from replica >= 9.6 we do not trust minRecPoint
		 * and stop_backup LSN, so we use latest replayed LSN as STOP LSN.
		 */
		stop_backup_on_replica_query[] =
			"SELECT"
			" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
			" current_timestamp(0)::timestamptz,"
			" pg_catalog.pg_last_wal_replay_lsn(),"
			" labelfile,"
			" spcmapfile"
			" FROM pg_catalog.pg_stop_backup(false, false)",
		stop_backup_on_replica_before10_query[] =
			"SELECT"
			" pg_catalog.txid_snapshot_xmax(pg_catalog.txid_current_snapshot()),"
			" current_timestamp(0)::timestamptz,"
			" pg_catalog.pg_last_xlog_replay_location(),"
			" labelfile,"
			" spcmapfile"
			" FROM pg_catalog.pg_stop_backup(false)";

	const char * const stop_backup_query =
		is_exclusive ?
			stop_exlusive_backup_query :
			server_version >= 100000 ?
				(is_started_on_replica ?
					stop_backup_on_replica_query :
					stop_backup_on_master_query
				) :
				(is_started_on_replica ?
					stop_backup_on_replica_before10_query :
					stop_backup_on_master_before10_query
				);
	bool		sent = false;

	/* Make proper timestamp format for parse_time(recovery_time) */
	pgut_execute(conn, "SET datestyle = 'ISO, DMY';", 0, NULL);
	// TODO: check result

	/*
	 * send pg_stop_backup asynchronously because we could came
	 * here from backup_cleanup() after some error caused by
	 * postgres archive_command problem and in this case we will
	 * wait for pg_stop_backup() forever.
	 */
	sent = pgut_send(conn, stop_backup_query, 0, NULL, WARNING);
	if (!sent)
		elog(ERROR, "Failed to send pg_stop_backup query");

	/* After we have sent pg_stop_backup, we don't need this callback anymore */
	pgut_atexit_pop(backup_stopbackup_callback, &stop_callback_params);

	if (query_text)
		*query_text = pgut_strdup(stop_backup_query);
}

/*
 * pg_stop_backup_consume -- get 'pg_stop_backup' query results
 * side effects:
 *  - allocates memory for tablespace_map and backup_label contents, so it must freed by caller (if its not null)
 * parameters:
 *  -
 */
void
pg_stop_backup_consume(PGconn *conn, int server_version,
		bool is_exclusive, uint32 timeout, const char *query_text,
		PGStopBackupResult *result)
{
	PGresult	*query_result;
	uint32		 pg_stop_backup_timeout = 0;
	enum stop_backup_query_result_column_numbers {
		recovery_xid_colno = 0,
		recovery_time_colno,
		lsn_colno,
		backup_label_colno,
		tablespace_map_colno
		};

	/* and now wait */
	while (1)
	{
		if (!PQconsumeInput(conn))
			elog(ERROR, "pg_stop backup() failed: %s",
					PQerrorMessage(conn));

		if (PQisBusy(conn))
		{
			pg_stop_backup_timeout++;
			sleep(1);

			if (interrupted)
			{
				pgut_cancel(conn);
				elog(ERROR, "interrupted during waiting for pg_stop_backup");
			}

			if (pg_stop_backup_timeout == 1)
				elog(INFO, "wait for pg_stop_backup()");

			/*
			 * If postgres haven't answered in archive_timeout seconds,
			 * send an interrupt.
			 */
			if (pg_stop_backup_timeout > timeout)
			{
				pgut_cancel(conn);
				elog(ERROR, "pg_stop_backup doesn't answer in %d seconds, cancel it", timeout);
			}
		}
		else
		{
			query_result = PQgetResult(conn);
			break;
		}
	}

	/* Check successfull execution of pg_stop_backup() */
	if (!query_result)
		elog(ERROR, "pg_stop_backup() failed");
	else
	{
		switch (PQresultStatus(query_result))
		{
			/*
			 * We should expect only PGRES_TUPLES_OK since pg_stop_backup
			 * returns tuples.
			 */
			case PGRES_TUPLES_OK:
				break;
			default:
				elog(ERROR, "query failed: %s query was: %s",
					 PQerrorMessage(conn), query_text);
		}
		backup_in_progress = false;
		elog(INFO, "pg_stop backup() successfully executed");
	}

	/* get results and fill result structure */
	/* get&check recovery_xid */
	if (sscanf(PQgetvalue(query_result, 0, recovery_xid_colno), XID_FMT, &result->snapshot_xid) != 1)
		elog(ERROR,
			 "result of txid_snapshot_xmax() is invalid: %s",
			 PQgetvalue(query_result, 0, recovery_xid_colno));

	/* get&check recovery_time */
	if (!parse_time(PQgetvalue(query_result, 0, recovery_time_colno), &result->invocation_time, true))
		elog(ERROR,
			 "result of current_timestamp is invalid: %s",
			 PQgetvalue(query_result, 0, recovery_time_colno));

	/* get stop_backup_lsn */
	{
		uint32	lsn_hi;
		uint32	lsn_lo;

//		char *target_lsn = "2/F578A000";
//		XLogDataFromLSN(target_lsn, &lsn_hi, &lsn_lo);

		/* Extract timeline and LSN from results of pg_stop_backup() */
		XLogDataFromLSN(PQgetvalue(query_result, 0, lsn_colno), &lsn_hi, &lsn_lo);
		/* Calculate LSN */
		result->lsn = ((uint64) lsn_hi) << 32 | lsn_lo;
	}

	/* get backup_label_content */
	result->backup_label_content = NULL;
	// if (!PQgetisnull(query_result, 0, backup_label_colno))
	if (!is_exclusive)
	{
		result->backup_label_content_len = PQgetlength(query_result, 0, backup_label_colno);
		if (result->backup_label_content_len > 0)
			result->backup_label_content = pgut_strndup(PQgetvalue(query_result, 0, backup_label_colno),
								result->backup_label_content_len);
	} else {
		result->backup_label_content_len = 0;
	}

	/* get tablespace_map_content */
	result->tablespace_map_content = NULL;
	// if (!PQgetisnull(query_result, 0, tablespace_map_colno))
	if (!is_exclusive)
	{
		result->tablespace_map_content_len = PQgetlength(query_result, 0, tablespace_map_colno);
		if (result->tablespace_map_content_len > 0)
			result->tablespace_map_content = pgut_strndup(PQgetvalue(query_result, 0, tablespace_map_colno),
								result->tablespace_map_content_len);
	} else {
		result->tablespace_map_content_len = 0;
	}
}

/*
 * helper routine used to write backup_label and tablespace_map in pg_stop_backup()
 */
void
pg_stop_backup_write_file_helper(const char *path, const char *filename, const char *error_msg_filename,
		const void *data, size_t len, parray *file_list)
{
	FILE	*fp;
	pgFile	*file;
	char	full_filename[MAXPGPATH];

	join_path_components(full_filename, path, filename);
	fp = fio_fopen(full_filename, PG_BINARY_W, FIO_BACKUP_HOST);
	if (fp == NULL)
		elog(ERROR, "can't open %s file \"%s\": %s",
			 error_msg_filename, full_filename, strerror(errno));

	if (fio_fwrite(fp, data, len) != len ||
		fio_fflush(fp) != 0 ||
		fio_fclose(fp))
		elog(ERROR, "can't write %s file \"%s\": %s",
			 error_msg_filename, full_filename, strerror(errno));

	/*
	 * It's vital to check if files_list is initialized,
	 * because we could get here because the backup was interrupted
	 */
	if (file_list)
	{
		file = pgFileNew(full_filename, filename, true, 0,
						 FIO_BACKUP_HOST);

		if (S_ISREG(file->mode))
		{
			file->crc = pgFileGetCRC(full_filename, true, false);

			file->write_size = file->size;
			file->uncompressed_size = file->size;
		}
		parray_append(file_list, file);
	}
}

/*
 * Notify end of backup to PostgreSQL server.
 */
static void
pg_stop_backup(InstanceState *instanceState, pgBackup *backup, PGconn *pg_startbackup_conn,
				PGNodeInfo *nodeInfo)
{
	PGStopBackupResult	stop_backup_result;
	char	*xlog_path, stream_xlog_path[MAXPGPATH];
	/* kludge against some old bug in archive_timeout. TODO: remove in 3.0.0 */
	int	     timeout = (instance_config.archive_timeout > 0) ?
				instance_config.archive_timeout : ARCHIVE_TIMEOUT_DEFAULT;
	char    *query_text = NULL;

	/* Remove it ? */
	if (!backup_in_progress)
		elog(ERROR, "backup is not in progress");

	pg_silent_client_messages(pg_startbackup_conn);

	/* Create restore point
	 * Only if backup is from master.
	 * For PG 9.5 create restore point only if pguser is superuser.
	 */
	if (!backup->from_replica &&
		!(nodeInfo->server_version < 90600 &&
		  !nodeInfo->is_superuser)) //TODO: check correctness
		pg_create_restore_point(pg_startbackup_conn, backup->start_time);

	/* Execute pg_stop_backup using PostgreSQL connection */
	pg_stop_backup_send(pg_startbackup_conn, nodeInfo->server_version, backup->from_replica, exclusive_backup, &query_text);

	/*
	 * Wait for the result of pg_stop_backup(), but no longer than
	 * archive_timeout seconds
	 */
	pg_stop_backup_consume(pg_startbackup_conn, nodeInfo->server_version, exclusive_backup, timeout, query_text, &stop_backup_result);

	if (backup->stream)
	{
		join_path_components(stream_xlog_path, backup->database_dir, PG_XLOG_DIR);
		xlog_path = stream_xlog_path;
	}
	else
		xlog_path = instanceState->instance_wal_subdir_path;

	wait_wal_and_calculate_stop_lsn(xlog_path, stop_backup_result.lsn, backup);

	/* Write backup_label and tablespace_map */
	if (!exclusive_backup)
	{
		Assert(stop_backup_result.backup_label_content != NULL);

		/* Write backup_label */
		pg_stop_backup_write_file_helper(backup->database_dir, PG_BACKUP_LABEL_FILE, "backup label",
			stop_backup_result.backup_label_content, stop_backup_result.backup_label_content_len,
			backup_files_list);
		free(stop_backup_result.backup_label_content);
		stop_backup_result.backup_label_content = NULL;
		stop_backup_result.backup_label_content_len = 0;

		/* Write tablespace_map */
		if (stop_backup_result.tablespace_map_content != NULL)
		{
			pg_stop_backup_write_file_helper(backup->database_dir, PG_TABLESPACE_MAP_FILE, "tablespace map",
				stop_backup_result.tablespace_map_content, stop_backup_result.tablespace_map_content_len,
				backup_files_list);
			free(stop_backup_result.tablespace_map_content);
			stop_backup_result.tablespace_map_content = NULL;
			stop_backup_result.tablespace_map_content_len = 0;
		}
	}

	if (backup->stream)
	{
		/* This function will also add list of xlog files
		 * to the passed filelist */
		if(wait_WAL_streaming_end(backup_files_list))
			elog(ERROR, "WAL streaming failed");
	}

	backup->recovery_xid = stop_backup_result.snapshot_xid;

	elog(LOG, "Getting the Recovery Time from WAL");

	/* iterate over WAL from stop_backup lsn to start_backup lsn */
	if (!read_recovery_info(xlog_path, backup->tli,
						instance_config.xlog_seg_size,
						backup->start_lsn, backup->stop_lsn,
						&backup->recovery_time))
	{
		elog(LOG, "Failed to find Recovery Time in WAL, forced to trust current_timestamp");
		backup->recovery_time = stop_backup_result.invocation_time;
	}

	/* Cleanup */
	pg_free(query_text);
}

/*
 * Notify end of backup to server when "backup_label" is in the root directory
 * of the DB cluster.
 * Also update backup status to ERROR when the backup is not finished.
 */
static void
backup_cleanup(bool fatal, void *userdata)
{
	/*
	 * Update status of backup in BACKUP_CONTROL_FILE to ERROR.
	 * end_time != 0 means backup finished
	 */
	if (current.status == BACKUP_STATUS_RUNNING && current.end_time == 0)
	{
		elog(WARNING, "Backup %s is running, setting its status to ERROR",
			 base36enc(current.start_time));
		current.end_time = time(NULL);
		current.status = BACKUP_STATUS_ERROR;
		write_backup(&current, true);
	}
}

/*
 * Take a backup of the PGDATA at a file level.
 * Copy all directories and files listed in backup_files_list.
 * If the file is 'datafile' (regular relation's main fork), read it page by page,
 * verify checksum and copy.
 * In incremental backup mode, copy only files or datafiles' pages changed after
 * previous backup.
 */
static void *
backup_files(void *arg)
{
	int			i;
	char		from_fullpath[MAXPGPATH];
	char		to_fullpath[MAXPGPATH];
	static time_t prev_time;

	backup_files_arg *arguments = (backup_files_arg *) arg;
	int 		n_backup_files_list = parray_num(arguments->files_list);

	prev_time = current.start_time;

	/* backup a file */
	for (i = 0; i < n_backup_files_list; i++)
	{
		pgFile	*file = (pgFile *) parray_get(arguments->files_list, i);
		pgFile	*prev_file = NULL;

		/* We have already copied all directories */
		if (S_ISDIR(file->mode))
			continue;

		if (arguments->thread_num == 1)
		{
			/* update backup_content.control every 60 seconds */
			if ((difftime(time(NULL), prev_time)) > 60)
			{
				write_backup_filelist(&current, arguments->files_list, arguments->from_root,
									  arguments->external_dirs, false);
				/* update backup control file to update size info */
				write_backup(&current, true);

				prev_time = time(NULL);
			}
		}

		if (!pg_atomic_test_set_flag(&file->lock))
			continue;

		/* check for interrupt */
		if (interrupted || thread_interrupted)
			elog(ERROR, "interrupted during backup");

		if (progress)
			elog(INFO, "Progress: (%d/%d). Process file \"%s\"",
				 i + 1, n_backup_files_list, file->rel_path);

		/* Handle zero sized files */
		if (file->size == 0)
		{
			file->write_size = 0;
			continue;
		}

		/* construct destination filepath */
		if (file->external_dir_num == 0)
		{
			join_path_components(from_fullpath, arguments->from_root, file->rel_path);
			join_path_components(to_fullpath, arguments->to_root, file->rel_path);
		}
		else
		{
			char 	external_dst[MAXPGPATH];
			char	*external_path = parray_get(arguments->external_dirs,
												file->external_dir_num - 1);

			makeExternalDirPathByNum(external_dst,
								 arguments->external_prefix,
								 file->external_dir_num);

			join_path_components(to_fullpath, external_dst, file->rel_path);
			join_path_components(from_fullpath, external_path, file->rel_path);
		}

		/* Encountered some strange beast */
		if (!S_ISREG(file->mode))
			elog(WARNING, "Unexpected type %d of file \"%s\", skipping",
							file->mode, from_fullpath);

		/* Check that file exist in previous backup */
		if (current.backup_mode != BACKUP_MODE_FULL)
		{
			pgFile	**prev_file_tmp = NULL;
			prev_file_tmp = (pgFile **) parray_bsearch(arguments->prev_filelist,
											file, pgFileCompareRelPathWithExternal);
			if (prev_file_tmp)
			{
				/* File exists in previous backup */
				file->exists_in_prev = true;
				prev_file = *prev_file_tmp;
			}
		}

		/* backup file */
		if (file->is_datafile && !file->is_cfs)
		{
			backup_data_file(file, from_fullpath, to_fullpath,
							 arguments->prev_start_lsn,
							 current.backup_mode,
							 instance_config.compress_alg,
							 instance_config.compress_level,
							 arguments->nodeInfo->checksum_version,
							 arguments->nodeInfo->ptrack_version_num,
							 arguments->nodeInfo->ptrack_schema,
							 arguments->hdr_map, false);
		}
		else
		{
			backup_non_data_file(file, prev_file, from_fullpath, to_fullpath,
								 current.backup_mode, current.parent_backup, true);
		}

		if (file->write_size == FILE_NOT_FOUND)
			continue;

		if (file->write_size == BYTES_INVALID)
		{
			elog(VERBOSE, "Skipping the unchanged file: \"%s\"", from_fullpath);
			continue;
		}

		elog(VERBOSE, "File \"%s\". Copied "INT64_FORMAT " bytes",
						from_fullpath, file->write_size);
	}

	/* ssh connection to longer needed */
	fio_disconnect();

	/* Data files transferring is successful */
	arguments->ret = 0;

	return NULL;
}

/*
 * Extract information about files in backup_list parsing their names:
 * - remove temp tables from the list
 * - remove unlogged tables from the list (leave the _init fork)
 * - set flags for database directories
 * - set flags for datafiles
 */
void
parse_filelist_filenames(parray *files, const char *root)
{
	size_t		i = 0;
	Oid			unlogged_file_reloid = 0;

	while (i < parray_num(files))
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);
		int 		sscanf_result;

		if (S_ISREG(file->mode) &&
			path_is_prefix_of_path(PG_TBLSPC_DIR, file->rel_path))
		{
			/*
			 * Found file in pg_tblspc/tblsOid/TABLESPACE_VERSION_DIRECTORY
			 * Legal only in case of 'pg_compression'
			 */
			if (strcmp(file->name, "pg_compression") == 0)
			{
				Oid			tblspcOid;
				Oid			dbOid;
				char		tmp_rel_path[MAXPGPATH];
				/*
				 * Check that the file is located under
				 * TABLESPACE_VERSION_DIRECTORY
				 */
				sscanf_result = sscanf(file->rel_path, PG_TBLSPC_DIR "/%u/%s/%u",
									   &tblspcOid, tmp_rel_path, &dbOid);

				/* Yes, it is */
				if (sscanf_result == 2 &&
					strncmp(tmp_rel_path, TABLESPACE_VERSION_DIRECTORY,
							strlen(TABLESPACE_VERSION_DIRECTORY)) == 0)
					set_cfs_datafiles(files, root, file->rel_path, i);
			}
		}

		if (S_ISREG(file->mode) && file->tblspcOid != 0 &&
			file->name && file->name[0])
		{
			if (file->forkName == init)
			{
				/*
				 * Do not backup files of unlogged relations.
				 * scan filelist backward and exclude these files.
				 */
				int			unlogged_file_num = i - 1;
				pgFile	   *unlogged_file = (pgFile *) parray_get(files,
																  unlogged_file_num);

				unlogged_file_reloid = file->relOid;

				while (unlogged_file_num >= 0 &&
					   (unlogged_file_reloid != 0) &&
					   (unlogged_file->relOid == unlogged_file_reloid))
				{
					pgFileFree(unlogged_file);
					parray_remove(files, unlogged_file_num);

					unlogged_file_num--;
					i--;

					unlogged_file = (pgFile *) parray_get(files,
														  unlogged_file_num);
				}
			}
		}

		i++;
	}
}

/* If file is equal to pg_compression, then we consider this tablespace as
 * cfs-compressed and should mark every file in this tablespace as cfs-file
 * Setting is_cfs is done via going back through 'files' set every file
 * that contain cfs_tablespace in his path as 'is_cfs'
 * Goings back through array 'files' is valid option possible because of current
 * sort rules:
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid/1
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/dboid/1.cfm
 * tblspcOid/TABLESPACE_VERSION_DIRECTORY/pg_compression
 */
static void
set_cfs_datafiles(parray *files, const char *root, char *relative, size_t i)
{
	int			len;
	int			p;
	pgFile	   *prev_file;
	char	   *cfs_tblspc_path;

	cfs_tblspc_path = strdup(relative);
	if(!cfs_tblspc_path)
		elog(ERROR, "Out of memory");
	len = strlen("/pg_compression");
	cfs_tblspc_path[strlen(cfs_tblspc_path) - len] = 0;
	elog(VERBOSE, "CFS DIRECTORY %s, pg_compression path: %s", cfs_tblspc_path, relative);

	for (p = (int) i; p >= 0; p--)
	{
		prev_file = (pgFile *) parray_get(files, (size_t) p);

		elog(VERBOSE, "Checking file in cfs tablespace %s", prev_file->rel_path);

		if (strstr(prev_file->rel_path, cfs_tblspc_path) != NULL)
		{
			if (S_ISREG(prev_file->mode) && prev_file->is_datafile)
			{
				elog(VERBOSE, "Setting 'is_cfs' on file %s, name %s",
					prev_file->rel_path, prev_file->name);
				prev_file->is_cfs = true;
			}
		}
		else
		{
			elog(VERBOSE, "Breaking on %s", prev_file->rel_path);
			break;
		}
	}
	free(cfs_tblspc_path);
}

/*
 * Find pgfile by given rnode in the backup_files_list
 * and add given blkno to its pagemap.
 */
void
process_block_change(ForkNumber forknum, RelFileNode rnode, BlockNumber blkno)
{
//	char	   *path;
	char	   *rel_path;
	BlockNumber blkno_inseg;
	int			segno;
	pgFile	  **file_item;
	pgFile		f;

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	rel_path = relpathperm(rnode, forknum);
	if (segno > 0)
		f.rel_path = psprintf("%s.%u", rel_path, segno);
	else
		f.rel_path = rel_path;

	f.external_dir_num = 0;

	/* backup_files_list should be sorted before */
	file_item = (pgFile **) parray_bsearch(backup_files_list, &f,
										   pgFileCompareRelPathWithExternal);

	/*
	 * If we don't have any record of this file in the file map, it means
	 * that it's a relation that did not have much activity since the last
	 * backup. We can safely ignore it. If it is a new relation file, the
	 * backup would simply copy it as-is.
	 */
	if (file_item)
	{
		/* We need critical section only we use more than one threads */
		if (num_threads > 1)
			pthread_lock(&backup_pagemap_mutex);

		datapagemap_add(&(*file_item)->pagemap, blkno_inseg);

		if (num_threads > 1)
			pthread_mutex_unlock(&backup_pagemap_mutex);
	}

	if (segno > 0)
		pg_free(f.rel_path);
	pg_free(rel_path);

}

static void
check_external_for_tablespaces(parray *external_list, PGconn *backup_conn)
{
	PGresult   *res;
	int			i = 0;
	int			j = 0;
	char	   *tablespace_path = NULL;
	char	   *query = "SELECT pg_catalog.pg_tablespace_location(oid) "
						"FROM pg_catalog.pg_tablespace "
						"WHERE pg_catalog.pg_tablespace_location(oid) <> '';";

	res = pgut_execute(backup_conn, query, 0, NULL);

	/* Check successfull execution of query */
	if (!res)
		elog(ERROR, "Failed to get list of tablespaces");

	for (i = 0; i < res->ntups; i++)
	{
		tablespace_path = PQgetvalue(res, i, 0);
		Assert (strlen(tablespace_path) > 0);

		canonicalize_path(tablespace_path);

		for (j = 0; j < parray_num(external_list); j++)
		{
			char *external_path = parray_get(external_list, j);

			if (path_is_prefix_of_path(external_path, tablespace_path))
				elog(ERROR, "External directory path (-E option) \"%s\" "
							"contains tablespace \"%s\"",
							external_path, tablespace_path);
			if (path_is_prefix_of_path(tablespace_path, external_path))
				elog(WARNING, "External directory path (-E option) \"%s\" "
							  "is in tablespace directory \"%s\"",
							  tablespace_path, external_path);
		}
	}
	PQclear(res);

	/* Check that external directories do not overlap */
	if (parray_num(external_list) < 2)
		return;

	for (i = 0; i < parray_num(external_list); i++)
	{
		char *external_path = parray_get(external_list, i);

		for (j = 0; j < parray_num(external_list); j++)
		{
			char *tmp_external_path = parray_get(external_list, j);

			/* skip yourself */
			if (j == i)
				continue;

			if (path_is_prefix_of_path(external_path, tmp_external_path))
				elog(ERROR, "External directory path (-E option) \"%s\" "
							"contain another external directory \"%s\"",
							external_path, tmp_external_path);

		}
	}
}
