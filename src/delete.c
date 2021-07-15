/*-------------------------------------------------------------------------
 *
 * delete.c: delete backup files.
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <dirent.h>
#include <time.h>
#include <unistd.h>

static void delete_walfiles_in_tli(XLogRecPtr keep_lsn, timelineInfo *tli,
						uint32 xlog_seg_size, bool dry_run);
static void do_retention_internal(parray *backup_list, parray *to_keep_list,
									parray *to_purge_list);
static void do_retention_merge(parray *backup_list, parray *to_keep_list,
									parray *to_purge_list, bool no_validate, bool no_sync);
static void do_retention_purge(parray *to_keep_list, parray *to_purge_list);
static void do_retention_wal(bool dry_run);

// TODO: more useful messages for dry run.
static bool backup_deleted = false;   /* At least one backup was deleted */
static bool backup_merged = false;    /* At least one merge was enacted */
static bool wal_deleted = false;      /* At least one WAL segments was deleted */

void
do_delete(time_t backup_id)
{
	int			i;
	parray	   *backup_list,
			   *delete_list;
	pgBackup   *target_backup = NULL;
	size_t		size_to_delete = 0;
	char		size_to_delete_pretty[20];

	/* Get complete list of backups */
	backup_list = catalog_get_backup_list(instance_name, INVALID_BACKUP_ID);

	delete_list = parray_new();

	/* Find backup to be deleted and make increment backups array to be deleted */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

		if (backup->start_time == backup_id)
		{
			target_backup = backup;
			break;
		}
	}

	/* sanity */
	if (!target_backup)
		elog(ERROR, "Failed to find backup %s, cannot delete", base36enc(backup_id));

	/* form delete list */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

		/* check if backup is descendant of delete target */
		if (is_parent(target_backup->start_time, backup, true))
		{
			parray_append(delete_list, backup);

			elog(LOG, "Backup %s %s be deleted",
				base36enc(backup->start_time), dry_run? "can":"will");

			size_to_delete += backup->data_bytes;
			if (backup->stream)
				size_to_delete += backup->wal_bytes;
		}
	}

	/* Report the resident size to delete */
	if (size_to_delete >= 0)
	{
		pretty_size(size_to_delete, size_to_delete_pretty, lengthof(size_to_delete_pretty));
		elog(INFO, "Resident data size to free by delete of backup %s : %s",
			base36enc(target_backup->start_time), size_to_delete_pretty);
	}

	if (!dry_run)
	{
		/* Lock marked for delete backups */
		catalog_lock_backup_list(delete_list, parray_num(delete_list) - 1, 0, false, true);

		/* Delete backups from the end of list */
		for (i = (int) parray_num(delete_list) - 1; i >= 0; i--)
		{
			pgBackup   *backup = (pgBackup *) parray_get(delete_list, (size_t) i);

			if (interrupted)
				elog(ERROR, "interrupted during delete backup");

			delete_backup_files(backup);
		}
	}

	/* Clean WAL segments */
	if (delete_wal)
		do_retention_wal(dry_run);

	/* cleanup */
	parray_free(delete_list);
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);
}

void
do_detach(time_t backup_id)
{
	int			i;
	parray	   *backup_list,
			   *delete_list;
	pgBackup   *target_backup = NULL;
	size_t		size_to_delete = 0;
	char		size_to_delete_pretty[20];

	/* Get complete list of backups */
	backup_list = catalog_get_backup_list(instance_name, INVALID_BACKUP_ID);

	delete_list = parray_new();

	/* Find backup to be deleted and make increment backups array to be deleted */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

		if (backup->start_time == backup_id)
		{
			target_backup = backup;
			break;
		}
	}

	/* sanity */
	if (!target_backup)
		elog(ERROR, "Failed to find backup %s, cannot detach", base36enc(backup_id));

	/* form delete list */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

		/* check if backup is descendant of delete target */
		if (is_parent(target_backup->start_time, backup, true))
		{
			parray_append(delete_list, backup);

			elog(LOG, "Backup %s %s be detached",
				base36enc(backup->start_time), dry_run? "can":"will");

			size_to_delete += backup->data_bytes;
			if (backup->stream)
				size_to_delete += backup->wal_bytes;
		}
	}

	/* Report the resident size to delete */
	if (size_to_delete >= 0)
	{
		pretty_size(size_to_delete, size_to_delete_pretty, lengthof(size_to_delete_pretty));
		elog(INFO, "Resident data size to free by detach of backup %s : %s",
			base36enc(target_backup->start_time), size_to_delete_pretty);
	}

	if (!dry_run)
	{
		/* Lock marked for delete backups */
		catalog_lock_backup_list(delete_list, parray_num(delete_list) - 1, 0, false, true);

		/* Delete backups from the end of list */
		for (i = (int) parray_num(delete_list) - 1; i >= 0; i--)
		{
			pgBackup   *backup = (pgBackup *) parray_get(delete_list, (size_t) i);

			if (interrupted)
				elog(ERROR, "interrupted during detach backup");

			detach_backup_files(backup);
		}
	}


	/* cleanup */
	parray_free(delete_list);
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);
}

/*
* Detach backup files of the backup and update the status of the backup to
* BACKUP_STATUS_DETACHED.
* TODO: delete files on multiple threads
*/
void
detach_backup_files(pgBackup *backup)
{
	size_t		i;
	char		timestamp[100];
	parray		*files;
	size_t		num_files;
	char		full_path[MAXPGPATH];

	/*
 	 * If the backup was detached already, there is nothing to do.
	 */
	if (backup->status == BACKUP_STATUS_DETACHED)
	{
		elog(WARNING, "Backup %s already detached",
			 base36enc(backup->start_time));
		return;
	}

	if (backup->recovery_time)
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time, false);
	else
		time2iso(timestamp, lengthof(timestamp), backup->start_time, false);

	elog(INFO, "Detach: %s %s",
		 base36enc(backup->start_time), timestamp);
        elog(INFO, "Backup paths:\n root_dir = %s\n database_dir = %s\n",
                 backup->root_dir, backup->database_dir);


	/*
 	 * Update STATUS to BACKUP_STATUS_DETACHING in preparation for the case which
 	 * the error occurs before deleting all backup files.
 	 */
	write_backup_status(backup, BACKUP_STATUS_DETACHING, instance_name, false);
        elog(INFO, "Set status in control file: DETACHING");

	/* list files to be deleted */
	files = parray_new();
	dir_list_file(files, backup->database_dir, false, false, true, false, false, 0, FIO_BACKUP_HOST);

	/* delete leaf node first */
	parray_qsort(files, pgFileCompareRelPathWithExternalDesc);
	num_files = parray_num(files);
	for (i = 0; i < num_files; i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		join_path_components(full_path, backup->database_dir, file->rel_path);

		if (interrupted)
			elog(ERROR, "interrupted during detach backup");

		if (progress)
			elog(INFO, "Progress: (%zd/%zd). Delete file \"%s\"",
				 i + 1, num_files, full_path);

		pgFileDelete(file->mode, full_path);
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
	backup->status = BACKUP_STATUS_DETACHED;

	/* Update STATUS to BACKUP_STATUS_DETACHED */
	write_backup_status(backup, BACKUP_STATUS_DETACHED, instance_name, true);
	elog(INFO, "Set status in control file: DETACHED");

	return;
}


/*
 * Merge and purge backups by retention policy. Retention policy is configured by
 * retention_redundancy and retention_window variables.
 *
 * Invalid backups handled in Oracle style, so invalid backups are ignored
 * for the purpose of retention fulfillment,
 * i.e. CORRUPT full backup do not taken in account when determine
 * which FULL backup should be keeped for redundancy obligation(only valid do),
 * but if invalid backup is not guarded by retention - it is removed
 */
void do_retention(bool no_validate, bool no_sync)
{
	parray	   *backup_list = NULL;
	parray	   *to_keep_list = parray_new();
	parray	   *to_purge_list = parray_new();

	bool	retention_is_set = false; /* At least one retention policy is set */
	bool 	backup_list_is_empty = false;

	backup_deleted = false;
	backup_merged = false;

	/* For now retention is possible only locally */
	MyLocation = FIO_LOCAL_HOST;

	/* Get a complete list of backups. */
	backup_list = catalog_get_backup_list(instance_name, INVALID_BACKUP_ID);

	if (parray_num(backup_list) == 0)
		backup_list_is_empty = true;

	if (delete_expired || merge_expired)
	{
		if (instance_config.retention_redundancy > 0)
			elog(LOG, "REDUNDANCY=%u", instance_config.retention_redundancy);
		if (instance_config.retention_window > 0)
			elog(LOG, "WINDOW=%u", instance_config.retention_window);

		if (instance_config.retention_redundancy == 0 &&
			instance_config.retention_window == 0)
		{
			/* Retention is disabled but we still can cleanup wal */
			elog(WARNING, "Retention policy is not set");
			if (!delete_wal)
				return;
		}
		else
			/* At least one retention policy is active */
			retention_is_set = true;
	}

	if (retention_is_set && backup_list_is_empty)
		elog(WARNING, "Backup list is empty, retention purge and merge are problematic");

	/* Populate purge and keep lists, and show retention state messages */
	if (retention_is_set && !backup_list_is_empty)
		do_retention_internal(backup_list, to_keep_list, to_purge_list);

	if (merge_expired && !dry_run && !backup_list_is_empty)
		do_retention_merge(backup_list, to_keep_list, to_purge_list, no_validate, no_sync);

	if (delete_expired && !dry_run && !backup_list_is_empty)
		do_retention_purge(to_keep_list, to_purge_list);

	/* TODO: some sort of dry run for delete_wal */
	if (delete_wal)
		do_retention_wal(dry_run);

	/* TODO: consider dry-run flag */

	if (!backup_merged)
		elog(INFO, "There are no backups to merge by retention policy");

	if (backup_deleted)
		elog(INFO, "Purging finished");
	else
		elog(INFO, "There are no backups to delete by retention policy");

	if (!wal_deleted)
		elog(INFO, "There is no WAL to purge by retention policy");

	/* Cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);
	parray_free(to_keep_list);
	parray_free(to_purge_list);
}

/* Evaluate every backup by retention policies and populate purge and keep lists.
 * Also for every backup print its status ('Active' or 'Expired') according
 * to active retention policies.
 */
static void
do_retention_internal(parray *backup_list, parray *to_keep_list, parray *to_purge_list)
{
	int			i;

	parray *redundancy_full_backup_list = NULL;

	/* For retention calculation */
	uint32		n_full_backups = 0;
	int			cur_full_backup_num = 0;
	time_t		days_threshold = 0;

	/* For fancy reporting */
	uint32		actual_window = 0;

	/* Calculate n_full_backups and days_threshold */
	if (instance_config.retention_redundancy > 0)
	{
		for (i = 0; i < parray_num(backup_list); i++)
		{
			pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

			if (backup->backup_mode == BACKUP_MODE_FULL)
			{
				/* Add every FULL backup that satisfy Redundancy policy to separate list */
				if (n_full_backups < instance_config.retention_redundancy)
				{
					if (!redundancy_full_backup_list)
						redundancy_full_backup_list = parray_new();

					parray_append(redundancy_full_backup_list, backup);
				}

				/* Consider only valid FULL backups for Redundancy fulfillment */
				if (backup->status == BACKUP_STATUS_OK ||
					backup->status == BACKUP_STATUS_DONE)
				{
					n_full_backups++;
				}
			}
		}
		/* Sort list of full backups to keep */
		if (redundancy_full_backup_list)
			parray_qsort(redundancy_full_backup_list, pgBackupCompareIdDesc);
	}

	if (instance_config.retention_window > 0)
	{
		days_threshold = current_time -
		(instance_config.retention_window * 60 * 60 * 24);
	}

	elog(INFO, "Evaluate backups by retention");
	for (i = (int) parray_num(backup_list) - 1; i >= 0; i--)
	{

		bool redundancy_keep = false;
		time_t backup_time = 0;
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, (size_t) i);

		/* check if backup`s FULL ancestor is in redundancy list */
		if (redundancy_full_backup_list)
		{
			pgBackup   *full_backup = find_parent_full_backup(backup);

			if (full_backup && parray_bsearch(redundancy_full_backup_list,
											  full_backup,
											  pgBackupCompareIdDesc))
				redundancy_keep = true;
		}

		/* Remember the serial number of latest valid FULL backup */
		if (backup->backup_mode == BACKUP_MODE_FULL &&
			(backup->status == BACKUP_STATUS_OK ||
			 backup->status == BACKUP_STATUS_DONE))
		{
			cur_full_backup_num++;
		}

		/* Invalid and running backups most likely to have recovery_time == 0,
		 * so in this case use start_time instead.
		 */
		if (backup->recovery_time)
			backup_time = backup->recovery_time;
		else
			backup_time = backup->start_time;

		/* Check if backup in needed by retention policy */
		if ((days_threshold == 0 || (days_threshold > backup_time)) &&
			(instance_config.retention_redundancy == 0 || !redundancy_keep))
		{
			/* This backup is not guarded by retention
			 *
			 * Redundancy = 1
			 * FULL CORRUPT in retention (not count toward redundancy limit)
			 * FULL  in retention
			 * ------retention redundancy -------
			 * PAGE3 in retention
			 * ------retention window -----------
			 * PAGE2 out of retention
			 * PAGE1 out of retention
			 * FULL  out of retention  	<- We are here
			 * FULL CORRUPT out of retention
			 */

			/* Save backup from purge if backup is pinned and
			 * expire date is not yet due.
			 */
			if ((backup->expire_time > 0) &&
				(backup->expire_time > current_time))
			{
				char		expire_timestamp[100];
				time2iso(expire_timestamp, lengthof(expire_timestamp), backup->expire_time, false);

				elog(LOG, "Backup %s is pinned until '%s', retain",
					base36enc(backup->start_time), expire_timestamp);
				continue;
			}

			/* Add backup to purge_list */
			elog(VERBOSE, "Mark backup %s for purge.", base36enc(backup->start_time));
			parray_append(to_purge_list, backup);
			continue;
		}
	}

	/* sort keep_list and purge list */
	parray_qsort(to_keep_list, pgBackupCompareIdDesc);
	parray_qsort(to_purge_list, pgBackupCompareIdDesc);

	/* FULL
	 * PAGE
	 * PAGE <- Only such backups must go into keep list
	 ---------retention window ----
	 * PAGE
	 * FULL
	 * PAGE
	 * FULL
	 */

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);

		/* Do not keep invalid backups by retention
		 * Turns out it was not a very good idea - [Issue #114]
		 */
		//if (backup->status != BACKUP_STATUS_OK &&
		//		backup->status != BACKUP_STATUS_DONE)
		//	continue;

		/* only incremental backups should be in keep list */
		if (backup->backup_mode == BACKUP_MODE_FULL)
			continue;

		/* orphan backup cannot be in keep list */
		if (!backup->parent_backup_link)
			continue;

		/* skip if backup already in purge list  */
		if (parray_bsearch(to_purge_list, backup, pgBackupCompareIdDesc))
			continue;

		/* if parent in purge_list, add backup to keep list */
		if (parray_bsearch(to_purge_list,
							backup->parent_backup_link,
							pgBackupCompareIdDesc))
		{
			/* make keep list a bit more compact */
			parray_append(to_keep_list, backup);
			continue;
		}
	}

	/* Message about retention state of backups
	 * TODO: message is ugly, rewrite it to something like show table in stdout.
	 */

	cur_full_backup_num = 1;
	for (i = 0; i < parray_num(backup_list); i++)
	{
		char		*action = "Active";
		uint32		pinning_window = 0;

		pgBackup	*backup = (pgBackup *) parray_get(backup_list, i);

		if (parray_bsearch(to_purge_list, backup, pgBackupCompareIdDesc))
			action = "Expired";

		if (backup->recovery_time == 0)
			actual_window = 0;
		else
			actual_window = (current_time - backup->recovery_time)/(3600 * 24);

		/* For pinned backups show expire date */
		if (backup->expire_time > 0 && backup->expire_time > backup->recovery_time)
			pinning_window = (backup->expire_time - backup->recovery_time)/(3600 * 24);

		/* TODO: add ancestor(chain full backup) ID */
		elog(INFO, "Backup %s, mode: %s, status: %s. Redundancy: %i/%i, Time Window: %ud/%ud. %s",
				base36enc(backup->start_time),
				pgBackupGetBackupMode(backup),
				status2str(backup->status),
				cur_full_backup_num,
				instance_config.retention_redundancy,
				actual_window,
				pinning_window ? pinning_window : instance_config.retention_window,
				action);

		/* Only valid full backups are count to something */
		if (backup->backup_mode == BACKUP_MODE_FULL &&
			(backup->status == BACKUP_STATUS_OK ||
			 backup->status == BACKUP_STATUS_DONE))
				cur_full_backup_num++;
	}
}

/* Merge partially expired incremental chains */
static void
do_retention_merge(parray *backup_list, parray *to_keep_list, parray *to_purge_list,
				   bool no_validate, bool no_sync)
{
	int i;
	int j;

	/* IMPORTANT: we can merge to only those FULL backup, that is NOT
	 * guarded by retention and final target of such merge must be
	 * an incremental backup that is guarded by retention !!!
	 *
	 *  PAGE4 E
	 *  PAGE3 D
	 --------retention window ---
	 *  PAGE2 C
	 *  PAGE1 B
	 *  FULL  A
	 *
	 * after retention merge:
	 * PAGE4 E
	 * FULL  D
	 */

	/* Merging happens here */
	for (i = 0; i < parray_num(to_keep_list); i++)
	{
		char		*keep_backup_id = NULL;
		pgBackup	*full_backup = NULL;
		parray	    *merge_list = NULL;

		pgBackup	*keep_backup = (pgBackup *) parray_get(to_keep_list, i);

		/* keep list may shrink during merge */
		if (!keep_backup)
			continue;

		elog(INFO, "Consider backup %s for merge", base36enc(keep_backup->start_time));

		/* Got valid incremental backup, find its FULL ancestor */
		full_backup = find_parent_full_backup(keep_backup);

		/* Failed to find parent */
		if (!full_backup)
		{
			elog(WARNING, "Failed to find FULL parent for %s", base36enc(keep_backup->start_time));
			continue;
		}

		/* Check that ancestor is in purge_list */
		if (!parray_bsearch(to_purge_list,
							full_backup,
							pgBackupCompareIdDesc))
		{
			elog(WARNING, "Skip backup %s for merging, "
				"because his FULL parent is not marked for purge", base36enc(keep_backup->start_time));
			continue;
		}

		/* FULL backup in purge list, thanks to compacting of keep_list current backup is
		 * final target for merge, but there could be intermediate incremental
		 * backups from purge_list.
		 */

		keep_backup_id = base36enc_dup(keep_backup->start_time);
		elog(INFO, "Merge incremental chain between full backup %s and backup %s",
					base36enc(full_backup->start_time), keep_backup_id);
		pg_free(keep_backup_id);

		merge_list = parray_new();

		/* Form up a merge list */
		while (keep_backup->parent_backup_link)
		{
			parray_append(merge_list, keep_backup);
			keep_backup = keep_backup->parent_backup_link;
		}

		/* sanity */
		if (!merge_list)
			continue;

		/* sanity */
		if (parray_num(merge_list) == 0)
		{
			parray_free(merge_list);
			continue;
		}

		/* In the end add FULL backup for easy locking */
		parray_append(merge_list, full_backup);

		/* Remove FULL backup from purge list */
		parray_rm(to_purge_list, full_backup, pgBackupCompareId);

		/* Lock merge chain */
		catalog_lock_backup_list(merge_list, parray_num(merge_list) - 1, 0, true, true);

		/* Consider this extreme case */
		//  PAGEa1    PAGEb1   both valid
		//      \     /
		//        FULL

		/* Check that FULL backup do not has multiple descendants
		 * full_backup always point to current full_backup after merge
		 */
//		if (is_prolific(backup_list, full_backup))
//		{
//			elog(WARNING, "Backup %s has multiple valid descendants. "
//					"Automatic merge is not possible.", base36enc(full_backup->start_time));
//		}

		/* Merge list example:
		 * 0 PAGE3
		 * 1 PAGE2
		 * 2 PAGE1
		 * 3 FULL
		 *
		 * Merge incremental chain from PAGE3 into FULL.
		 */

		keep_backup = parray_get(merge_list, 0);
		merge_chain(merge_list, full_backup, keep_backup, no_validate, no_sync);
		backup_merged = true;

		for (j = parray_num(merge_list) - 2; j >= 0; j--)
		{
			pgBackup   *tmp_backup = (pgBackup *) parray_get(merge_list, j);

			/* Try to remove merged incremental backup from both keep and purge lists */
			parray_rm(to_purge_list, tmp_backup, pgBackupCompareId);
			parray_set(to_keep_list, i, NULL);
		}
		if (!no_validate)
			pgBackupValidate(full_backup, NULL);
		if (full_backup->status == BACKUP_STATUS_CORRUPT)
			elog(ERROR, "Merging of backup %s failed", base36enc(full_backup->start_time));

		/* Cleanup */
		parray_free(merge_list);
	}

	elog(INFO, "Retention merging finished");

}

/* Purge expired backups */
static void
do_retention_purge(parray *to_keep_list, parray *to_purge_list)
{
	int i;
	int j;

	/* Remove backups by retention policy. Retention policy is configured by
	 * retention_redundancy and retention_window
	 * Remove only backups, that do not have children guarded by retention
	 *
	 * TODO: We do not consider the situation if child is marked for purge
	 * but parent isn`t. Maybe something bad happened with time on server?
	 */

	for (j = 0; j < parray_num(to_purge_list); j++)
	{
		bool purge = true;

		pgBackup   *delete_backup = (pgBackup *) parray_get(to_purge_list, j);

		elog(LOG, "Consider backup %s for purge",
						base36enc(delete_backup->start_time));

		/* Evaluate marked for delete backup against every backup in keep list.
		 * If marked for delete backup is recognized as parent of one of those,
		 * then this backup should not be deleted.
		 */
		for (i = 0; i < parray_num(to_keep_list); i++)
		{
			char		*keeped_backup_id;

			pgBackup   *keep_backup = (pgBackup *) parray_get(to_keep_list, i);

			/* item could have been nullified in merge */
			if (!keep_backup)
				continue;

			/* Full backup cannot be a descendant */
			if (keep_backup->backup_mode == BACKUP_MODE_FULL)
				continue;

			keeped_backup_id = base36enc_dup(keep_backup->start_time);

			elog(LOG, "Check if backup %s is parent of backup %s",
						base36enc(delete_backup->start_time), keeped_backup_id);

			if (is_parent(delete_backup->start_time, keep_backup, true))
			{

				/* We must not delete this backup, evict it from purge list */
				elog(LOG, "Retain backup %s because his "
					"descendant %s is guarded by retention",
						base36enc(delete_backup->start_time), keeped_backup_id);

				purge = false;
				pg_free(keeped_backup_id);
				break;
			}
			pg_free(keeped_backup_id);
		}

		/* Retain backup */
		if (!purge)
			continue;

		/* Actual purge */
		if (!lock_backup(delete_backup, false, true))
		{
			/* If the backup still is used, do not interrupt and go to the next */
			elog(WARNING, "Cannot lock backup %s directory, skip purging",
				 base36enc(delete_backup->start_time));
			continue;
		}

		/* Delete backup and update status to DELETED */
		delete_backup_files(delete_backup);
		backup_deleted = true;

	}
}

/*
 * Purge WAL
 * Iterate over timelines
 * Look for WAL segment not reachable from existing backups
 * and delete them.
 */
static void
do_retention_wal(bool dry_run)
{
	parray 		*tli_list;
	int i;

	tli_list = catalog_get_timelines(&instance_config);

	for (i = 0; i < parray_num(tli_list); i++)
	{
		timelineInfo  *tlinfo = (timelineInfo  *) parray_get(tli_list, i);

		/*
		 * Empty timeline (only mentioned in timeline history file)
		 * has nothing to cleanup.
		 */
		if (tlinfo->n_xlog_files == 0 && parray_num(tlinfo->xlog_filelist) == 0)
			continue;

		/*
		 * If closest backup exists, then timeline is reachable from
		 * at least one backup and no file should be removed.
		 * Unless wal-depth is enabled.
		 */
		if ((tlinfo->closest_backup) && instance_config.wal_depth <= 0)
			continue;

		/* WAL retention keeps this timeline from purge */
		if (instance_config.wal_depth >= 0 && tlinfo->anchor_tli > 0 &&
			tlinfo->anchor_tli != tlinfo->tli)
			continue;

		/*
		 * Purge all WAL segments before START LSN of oldest backup.
		 * If timeline doesn't have a backup, then whole timeline
		 * can be safely purged.
		 * Note, that oldest_backup is not necessarily valid here,
		 * but still we keep wal for it.
		 * If wal-depth is enabled then use anchor_lsn instead
		 * of oldest_backup.
		 */
		if (tlinfo->oldest_backup)
		{
			if (instance_config.wal_depth >= 0 && !(XLogRecPtrIsInvalid(tlinfo->anchor_lsn)))
			{
				delete_walfiles_in_tli(tlinfo->anchor_lsn,
								tlinfo, instance_config.xlog_seg_size, dry_run);
			}
			else
			{
				delete_walfiles_in_tli(tlinfo->oldest_backup->start_lsn,
								tlinfo, instance_config.xlog_seg_size, dry_run);
			}
		}
		else
		{
			if (instance_config.wal_depth >= 0 && !(XLogRecPtrIsInvalid(tlinfo->anchor_lsn)))
				delete_walfiles_in_tli(tlinfo->anchor_lsn,
								tlinfo, instance_config.xlog_seg_size, dry_run);
			else
				delete_walfiles_in_tli(InvalidXLogRecPtr,
								tlinfo, instance_config.xlog_seg_size, dry_run);
		}
	}
}

/*
 * Delete backup files of the backup and update the status of the backup to
 * BACKUP_STATUS_DELETED.
 * TODO: delete files on multiple threads
 */
void
delete_backup_files(pgBackup *backup)
{
	size_t		i;
	char		timestamp[100];
	parray		*files;
	size_t		num_files;
	char		full_path[MAXPGPATH];

	/*
	 * If the backup was deleted already, there is nothing to do.
	 */
	if (backup->status == BACKUP_STATUS_DELETED)
	{
		elog(WARNING, "Backup %s already deleted",
			 base36enc(backup->start_time));
		return;
	}

	if (backup->recovery_time)
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time, false);
	else
		time2iso(timestamp, lengthof(timestamp), backup->start_time, false);

	elog(INFO, "Delete: %s %s",
		 base36enc(backup->start_time), timestamp);

	/*
	 * Update STATUS to BACKUP_STATUS_DELETING in preparation for the case which
	 * the error occurs before deleting all backup files.
	 */
	write_backup_status(backup, BACKUP_STATUS_DELETING, instance_name, false);

	/* list files to be deleted */
	files = parray_new();
	dir_list_file(files, backup->root_dir, false, false, true, false, false, 0, FIO_BACKUP_HOST);

	/* delete leaf node first */
	parray_qsort(files, pgFileCompareRelPathWithExternalDesc);
	num_files = parray_num(files);
	for (i = 0; i < num_files; i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		join_path_components(full_path, backup->root_dir, file->rel_path);

		if (interrupted)
			elog(ERROR, "interrupted during delete backup");

		if (progress)
			elog(INFO, "Progress: (%zd/%zd). Delete file \"%s\"",
				 i + 1, num_files, full_path);

		pgFileDelete(file->mode, full_path);
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
	backup->status = BACKUP_STATUS_DELETED;

	return;
}

/*
 * Purge WAL archive.  One timeline at a time.
 * If 'keep_lsn' is InvalidXLogRecPtr, then whole timeline can be purged
 * If 'keep_lsn' is valid LSN, then every lesser segment can be purged.
 * If 'dry_run' is set, then don`t actually delete anything.
 *
 * Case 1:
 *	archive is not empty, 'keep_lsn' is valid and we can delete something.
 * Case 2:
 *	archive is not empty, 'keep_lsn' is valid and prevening us from deleting anything.
 * Case 3:
 * 	archive is not empty, 'keep_lsn' is invalid, drop all WAL files in archive,
 *												 belonging to the timeline.
 * Case 4:
 * 	archive is empty, 'keep_lsn' is valid, assume corruption of WAL archive.
 * Case 5:
 * 	archive is empty, 'keep_lsn' is invalid, drop backup history files
 *  and partial WAL segments in archive.
 *
 * Q: Maybe we should stop treating partial WAL segments as second-class citizens?
 */
static void
delete_walfiles_in_tli(XLogRecPtr keep_lsn, timelineInfo *tlinfo,
								uint32 xlog_seg_size, bool dry_run)
{
	XLogSegNo   FirstToDeleteSegNo;
	XLogSegNo   OldestToKeepSegNo = 0;
	char 		first_to_del_str[MAXFNAMELEN];
	char 		oldest_to_keep_str[MAXFNAMELEN];
	int			i;
	size_t		wal_size_logical = 0;
	size_t		wal_size_actual = 0;
	char		wal_pretty_size[20];
	bool		purge_all = false;


	/* Timeline is completely empty */
	if (parray_num(tlinfo->xlog_filelist) == 0)
	{
		elog(INFO, "Timeline %i is empty, nothing to remove", tlinfo->tli);
		return;
	}

	if (XLogRecPtrIsInvalid(keep_lsn))
	{
		/* Drop all files in timeline */
		elog(INFO, "On timeline %i all files %s be removed",
						tlinfo->tli, dry_run?"can":"will");
		FirstToDeleteSegNo = tlinfo->begin_segno;
		OldestToKeepSegNo = tlinfo->end_segno;
		purge_all = true;
	}
	else
	{
		/* Drop all segments between begin_segno and segment with keep_lsn (excluding) */
		FirstToDeleteSegNo = tlinfo->begin_segno;
		GetXLogSegNo(keep_lsn, OldestToKeepSegNo, xlog_seg_size);
	}

	if (OldestToKeepSegNo > 0 && OldestToKeepSegNo > FirstToDeleteSegNo)
	{
		/* translate segno number into human readable format */
		GetXLogFileName(first_to_del_str, tlinfo->tli, FirstToDeleteSegNo, xlog_seg_size);
		GetXLogFileName(oldest_to_keep_str, tlinfo->tli, OldestToKeepSegNo, xlog_seg_size);

		elog(INFO, "On timeline %i WAL segments between %s and %s %s be removed",
					 tlinfo->tli, first_to_del_str,
					 oldest_to_keep_str, dry_run?"can":"will");
	}

	/* sanity */
	if (OldestToKeepSegNo > FirstToDeleteSegNo)
	{
		wal_size_logical = (OldestToKeepSegNo - FirstToDeleteSegNo) * xlog_seg_size;

		/* In case of 'purge all' scenario OldestToKeepSegNo will be deleted too */
		if (purge_all)
			wal_size_logical += xlog_seg_size;
	}
	else if (OldestToKeepSegNo < FirstToDeleteSegNo)
	{
		/* It is actually possible for OldestToKeepSegNo to be less than FirstToDeleteSegNo
		 * in case of :
		 * 1. WAL archive corruption.
		 * 2. There is no actual WAL archive to speak of and
		 *	  'keep_lsn' is coming from STREAM backup.
		 */

		if (FirstToDeleteSegNo > 0 && OldestToKeepSegNo > 0)
		{
			GetXLogFileName(first_to_del_str, tlinfo->tli, FirstToDeleteSegNo, xlog_seg_size);
			GetXLogFileName(oldest_to_keep_str, tlinfo->tli, OldestToKeepSegNo, xlog_seg_size);

			elog(LOG, "On timeline %i first segment %s is greater than oldest segment to keep %s",
					tlinfo->tli, first_to_del_str, oldest_to_keep_str);
		}
	}
	else if (OldestToKeepSegNo == FirstToDeleteSegNo && !purge_all)
	{
		/* 'Nothing to delete' scenario because of 'keep_lsn'
		 * with possible exception of partial and backup history files.
		 */
		elog(INFO, "Nothing to remove on timeline %i", tlinfo->tli);
	}

	/* Report the logical size to delete */
	if (wal_size_logical > 0)
	{
		pretty_size(wal_size_logical, wal_pretty_size, lengthof(wal_pretty_size));
		elog(INFO, "Logical WAL size to remove on timeline %i : %s",
			tlinfo->tli, wal_pretty_size);
	}

	/* Calculate the actual size to delete */
	for (i = 0; i < parray_num(tlinfo->xlog_filelist); i++)
	{
		xlogFile *wal_file = (xlogFile *) parray_get(tlinfo->xlog_filelist, i);

		if (purge_all || wal_file->segno < OldestToKeepSegNo)
			wal_size_actual += wal_file->file.size;
	}

	/* Report the actual size to delete */
	if (wal_size_actual > 0)
	{
		pretty_size(wal_size_actual, wal_pretty_size, lengthof(wal_pretty_size));
		elog(INFO, "Resident WAL size to free on timeline %i : %s",
			tlinfo->tli, wal_pretty_size);
	}

	if (dry_run)
		return;

	for (i = 0; i < parray_num(tlinfo->xlog_filelist); i++)
	{
		xlogFile *wal_file = (xlogFile *) parray_get(tlinfo->xlog_filelist, i);

		if (interrupted)
			elog(ERROR, "interrupted during WAL archive purge");

		/* Any segment equal or greater than EndSegNo must be kept
		 * unless it`s a 'purge all' scenario.
		 */
		if (purge_all || wal_file->segno < OldestToKeepSegNo)
		{
			char wal_fullpath[MAXPGPATH];

			join_path_components(wal_fullpath, instance_config.arclog_path, wal_file->file.name);

			/* save segment from purging */
			if (instance_config.wal_depth >= 0 && wal_file->keep)
			{
				elog(VERBOSE, "Retain WAL segment \"%s\"", wal_fullpath);
				continue;
			}

			/* unlink segment */
			if (fio_unlink(wal_fullpath, FIO_BACKUP_HOST) < 0)
			{
				/* Missing file is not considered as error condition */
				if (errno != ENOENT)
					elog(ERROR, "Could not remove file \"%s\": %s",
							wal_fullpath, strerror(errno));
			}
			else
			{
				if (wal_file->type == SEGMENT)
					elog(VERBOSE, "Removed WAL segment \"%s\"", wal_fullpath);
				else if (wal_file->type == TEMP_SEGMENT)
					elog(VERBOSE, "Removed temp WAL segment \"%s\"", wal_fullpath);
				else if (wal_file->type == PARTIAL_SEGMENT)
					elog(VERBOSE, "Removed partial WAL segment \"%s\"", wal_fullpath);
				else if (wal_file->type == BACKUP_HISTORY_FILE)
					elog(VERBOSE, "Removed backup history file \"%s\"", wal_fullpath);
			}

			wal_deleted = true;
		}
	}
}


/* Delete all backup files and wal files of given instance. */
int
do_delete_instance(void)
{
	parray		*backup_list;
	int 		i;
	char		instance_config_path[MAXPGPATH];


	/* Delete all backups. */
	backup_list = catalog_get_backup_list(instance_name, INVALID_BACKUP_ID);

	catalog_lock_backup_list(backup_list, 0, parray_num(backup_list) - 1, true, true);

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);
		delete_backup_files(backup);
	}

	/* Cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);

	/* Delete all wal files. */
	pgut_rmtree(arclog_path, false, true);

	/* Delete backup instance config file */
	join_path_components(instance_config_path, backup_instance_path, BACKUP_CATALOG_CONF_FILE);
	if (remove(instance_config_path))
	{
		elog(ERROR, "Can't remove \"%s\": %s", instance_config_path,
			strerror(errno));
	}

	/* Delete instance root directories */
	if (rmdir(backup_instance_path) != 0)
		elog(ERROR, "Can't remove \"%s\": %s", backup_instance_path,
			strerror(errno));

	if (rmdir(arclog_path) != 0)
		elog(ERROR, "Can't remove \"%s\": %s", arclog_path,
			strerror(errno));

	elog(INFO, "Instance '%s' successfully deleted", instance_name);
	return 0;
}

/* Delete all backups of given status in instance */
void
do_delete_status(InstanceConfig *instance_config, const char *status)
{
	int         i;
	parray     *backup_list, *delete_list;
	const char *pretty_status;
	int         n_deleted = 0, n_found = 0;
	size_t      size_to_delete = 0;
	char        size_to_delete_pretty[20];
	pgBackup   *backup;

	BackupStatus status_for_delete = str2status(status);
	delete_list = parray_new();

	if (status_for_delete == BACKUP_STATUS_INVALID)
		elog(ERROR, "Unknown value for '--status' option: '%s'", status);

	/*
	 * User may have provided status string in lower case, but
	 * we should print backup statuses consistently with show command,
	 * so convert it.
	 */
	pretty_status = status2str(status_for_delete);

	backup_list = catalog_get_backup_list(instance_config->name, INVALID_BACKUP_ID);

	if (parray_num(backup_list) == 0)
	{
		elog(WARNING, "Instance '%s' has no backups", instance_config->name);
		return;
	}

	if (dry_run)
		elog(INFO, "Deleting all backups with status '%s' in dry run mode", pretty_status);
	else
		elog(INFO, "Deleting all backups with status '%s'", pretty_status);

	/* Selects backups with specified status and their children into delete_list array. */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		backup = (pgBackup *) parray_get(backup_list, i);

		if (backup->status == status_for_delete)
		{
			n_found++;

			/* incremental backup can be already in delete_list due to append_children() */
			if (parray_contains(delete_list, backup))
				continue;
			parray_append(delete_list, backup);

			append_children(backup_list, backup, delete_list);
		}
	}

	parray_qsort(delete_list, pgBackupCompareIdDesc);

	/* delete and calculate free size from delete_list */
	for (i = 0; i < parray_num(delete_list); i++)
	{
		backup = (pgBackup *)parray_get(delete_list, i);

		elog(INFO, "Backup %s with status %s %s be deleted",
			base36enc(backup->start_time), status2str(backup->status), dry_run ? "can" : "will");

		size_to_delete += backup->data_bytes;
		if (backup->stream)
			size_to_delete += backup->wal_bytes;

		if (!dry_run && lock_backup(backup, false, true))
			delete_backup_files(backup);

		n_deleted++;
	}

	/* Inform about data size to free */
	if (size_to_delete >= 0)
	{
		pretty_size(size_to_delete, size_to_delete_pretty, lengthof(size_to_delete_pretty));
		elog(INFO, "Resident data size to free by delete of %i backups: %s",
			n_deleted, size_to_delete_pretty);
	}

	/* delete selected backups */
	if (!dry_run && n_deleted > 0)
		elog(INFO, "Successfully deleted %i %s from instance '%s'",
			n_deleted, n_deleted == 1 ? "backup" : "backups",
			instance_config->name);


	if (n_found == 0)
		elog(WARNING, "Instance '%s' has no backups with status '%s'",
			instance_config->name, pretty_status);

	// we don`t do WAL purge here, because it is impossible to correctly handle
	// dry-run case.

	/* Cleanup */
	parray_free(delete_list);
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);
}
