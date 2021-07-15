/*-------------------------------------------------------------------------
 *
 * catalog.c: backup catalog operation
 *
 * Portions Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"
#include "access/timeline.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/file.h"
#include "utils/configuration.h"

static pgBackup* get_closest_backup(timelineInfo *tlinfo);
static pgBackup* get_oldest_backup(timelineInfo *tlinfo);
static const char *backupModes[] = {"", "PAGE", "PTRACK", "DELTA", "FULL"};
static pgBackup *readBackupControlFile(const char *path);
static time_t create_backup_dir(pgBackup *backup, const char *backup_instance_path);

static bool backup_lock_exit_hook_registered = false;
static parray *locks = NULL;

static int grab_excl_lock_file(const char *backup_dir, const char *backup_id, bool strict);
static int grab_shared_lock_file(pgBackup *backup);
static int wait_shared_owners(pgBackup *backup);

static void unlock_backup(const char *backup_dir, const char *backup_id, bool exclusive);
static void release_excl_lock_file(const char *backup_dir);
static void release_shared_lock_file(const char *backup_dir);

#define LOCK_OK            0
#define LOCK_FAIL_TIMEOUT  1
#define LOCK_FAIL_ENOSPC   2
#define LOCK_FAIL_EROFS    3

typedef struct LockInfo
{
	char backup_id[10];
	char backup_dir[MAXPGPATH];
	bool exclusive;
} LockInfo;

timelineInfo *
timelineInfoNew(TimeLineID tli)
{
	timelineInfo *tlinfo = (timelineInfo *) pgut_malloc(sizeof(timelineInfo));
	MemSet(tlinfo, 0, sizeof(timelineInfo));
	tlinfo->tli = tli;
	tlinfo->switchpoint = InvalidXLogRecPtr;
	tlinfo->parent_link = NULL;
	tlinfo->xlog_filelist = parray_new();
	tlinfo->anchor_lsn = InvalidXLogRecPtr;
	tlinfo->anchor_tli = 0;
	tlinfo->n_xlog_files = 0;
	return tlinfo;
}

/* free timelineInfo object */
void
timelineInfoFree(void *tliInfo)
{
	timelineInfo *tli = (timelineInfo *) tliInfo;

	parray_walk(tli->xlog_filelist, pgFileFree);
	parray_free(tli->xlog_filelist);

	if (tli->backups)
	{
		/* backups themselves should freed separately  */
//		parray_walk(tli->backups, pgBackupFree);
		parray_free(tli->backups);
	}

	pfree(tliInfo);
}

/* Iterate over locked backups and unlock them */
static void
unlink_lock_atexit(void)
{
	int	i;

	if (locks == NULL)
		return;

	for (i = 0; i < parray_num(locks); i++)
	{
		LockInfo *lock = (LockInfo *) parray_get(locks, i);
		unlock_backup(lock->backup_dir, lock->backup_dir, lock->exclusive);
	}

	parray_walk(locks, pg_free);
	parray_free(locks);
	locks = NULL;
}

/*
 * Read backup meta information from BACKUP_CONTROL_FILE.
 * If no backup matches, return NULL.
 */
pgBackup *
read_backup(const char *root_dir)
{
	char		conf_path[MAXPGPATH];

	join_path_components(conf_path, root_dir, BACKUP_CONTROL_FILE);

	return readBackupControlFile(conf_path);
}

/*
 * Save the backup status into BACKUP_CONTROL_FILE.
 *
 * We need to reread the backup using its ID and save it changing only its
 * status.
 */
void
write_backup_status(pgBackup *backup, BackupStatus status,
					const char *instance_name, bool strict)
{
	pgBackup   *tmp;

	tmp = read_backup(backup->root_dir);
	if (!tmp)
	{
		/*
		 * Silently exit the function, since read_backup already logged the
		 * warning message.
		 */
		return;
	}

	/* overwrite control file only if status has changed */
	if (tmp->status == status)
	{
		pgBackupFree(tmp);
		return;
	}

	backup->status = status;
	tmp->status = backup->status;
	tmp->root_dir = pgut_strdup(backup->root_dir);

	/* lock backup in exclusive mode */
	if (!lock_backup(tmp, strict, true))
		elog(ERROR, "Cannot lock backup %s directory", base36enc(backup->start_time));

	write_backup(tmp, strict);

	pgBackupFree(tmp);
}

/*
 * Lock backup in either exclusive or shared mode.
 * "strict" flag allows to ignore "out of space" errors and should be
 * used only by DELETE command to free disk space on filled up
 * filesystem.
 *
 * Only read only tasks (validate, restore) are allowed to take shared locks.
 * Changing backup metadata must be done with exclusive lock.
 *
 * Only one process can hold exclusive lock at any time.
 * Exlusive lock - PID of process, holding the lock - is placed in
 * lock file: BACKUP_LOCK_FILE.
 *
 * Multiple proccess are allowed to take shared locks simultaneously.
 * Shared locks - PIDs of proccesses, holding the lock - are placed in
 * separate lock file: BACKUP_RO_LOCK_FILE.
 * When taking shared lock, a brief exclusive lock is taken.
 *
 * -> exclusive -> grab exclusive lock file and wait until all shared lockers are gone, return
 * -> shared    -> grab exclusive lock file, grab shared lock file, release exclusive lock file, return
 *
 * TODO: lock-timeout as parameter
 */
bool
lock_backup(pgBackup *backup, bool strict, bool exclusive)
{
	int		  rc;
	char	  lock_file[MAXPGPATH];
	bool	  enospc_detected = false;
	LockInfo *lock = NULL;

	join_path_components(lock_file, backup->root_dir, BACKUP_LOCK_FILE);

	rc = grab_excl_lock_file(backup->root_dir, base36enc(backup->start_time), strict);

	if (rc == LOCK_FAIL_TIMEOUT)
		return false;
	else if (rc == LOCK_FAIL_ENOSPC)
	{
		/*
		 * If we failed to take exclusive lock due to ENOSPC,
		 * then in lax mode treat such condition as if lock was taken.
		 */

		enospc_detected = true;
		if (strict)
			return false;
	}
	else if (rc == LOCK_FAIL_EROFS)
	{
		/*
		 * If we failed to take exclusive lock due to EROFS,
		 * then in shared mode treat such condition as if lock was taken.
		 */
		return !exclusive;
	}

	/*
	 * We have exclusive lock, now there are following scenarios:
	 *
	 * 1. If we are for exlusive lock, then we must open the shared lock file
	 *    and check if any of the processes listed there are still alive.
	 *    If some processes are alive and are not going away in lock_timeout,
	 *    then return false.
	 *
	 * 2. If we are here for non-exlusive lock, then write the pid
	 *    into shared lock file and release the exclusive lock.
	 */

	if (exclusive)
		rc = wait_shared_owners(backup);
	else
		rc = grab_shared_lock_file(backup);

	if (rc != 0)
	{
		/*
		 * Failed to grab shared lock or (in case of exclusive mode) shared lock owners
		 * are not going away in time, release the exclusive lock file and return in shame.
		 */
		release_excl_lock_file(backup->root_dir);
		return false;
	}

	if (!exclusive)
	{
		/* Shared lock file is grabbed, now we can release exclusive lock file */
		release_excl_lock_file(backup->root_dir);
	}

	if (exclusive && !strict && enospc_detected)
	{
		/* We are in lax exclusive mode and EONSPC was encountered:
		 * once again try to grab exclusive lock file,
		 * because there is a chance that release of shared lock file in wait_shared_owners may have
		 * freed some space on filesystem, thanks to unlinking of BACKUP_RO_LOCK_FILE.
		 * If somebody concurrently acquired exclusive lock file first, then we should give up.
		 */
		if (grab_excl_lock_file(backup->root_dir, base36enc(backup->start_time), strict) == LOCK_FAIL_TIMEOUT)
			return false;

		return true;
	}

	/*
	 * Arrange the unlocking at proc_exit.
	 */
	if (!backup_lock_exit_hook_registered)
	{
		atexit(unlink_lock_atexit);
		backup_lock_exit_hook_registered = true;
	}

	/* save lock metadata for later unlocking */
	lock = pgut_malloc(sizeof(LockInfo));
	snprintf(lock->backup_id, 10, "%s", base36enc(backup->backup_id));
	snprintf(lock->backup_dir, MAXPGPATH, "%s", backup->root_dir);
	lock->exclusive = exclusive;

	/* Use parray for lock release */
	if (locks == NULL)
		locks = parray_new();
	parray_append(locks, lock);

	return true;
}

/*
 * Lock backup in exclusive mode
 * Result codes:
 *  LOCK_OK           Success
 *  LOCK_FAIL_TIMEOUT Failed to acquire lock in lock_timeout time
 *  LOCK_FAIL_ENOSPC  Failed to acquire lock due to ENOSPC
 *  LOCK_FAIL_EROFS   Failed to acquire lock due to EROFS
 */
int
grab_excl_lock_file(const char *root_dir, const char *backup_id, bool strict)
{
	char		lock_file[MAXPGPATH];
	int			fd = 0;
	char		buffer[256];
	int			ntries = LOCK_TIMEOUT;
	int			empty_tries = LOCK_STALE_TIMEOUT;
	int			len;
	int			encoded_pid;

	join_path_components(lock_file, root_dir, BACKUP_LOCK_FILE);

	/*
	 * We need a loop here because of race conditions.  But don't loop forever
	 * (for example, a non-writable $backup_instance_path directory might cause a failure
	 * that won't go away).
	 */
	do
	{
		FILE *fp_out = NULL;

		if (interrupted)
			elog(ERROR, "Interrupted while locking backup %s", backup_id);

		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600.  See
		 * comments below.
		 */
		fd = fio_open(lock_file, O_RDWR | O_CREAT | O_EXCL, FIO_BACKUP_HOST);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/* read-only fs is a special case */
		if (errno == EROFS)
		{
			elog(WARNING, "Could not create lock file \"%s\": %s",
				 lock_file, strerror(errno));
			return LOCK_FAIL_EROFS;
		}

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 * If file already exists or we have some permission problem (???),
		 * then retry;
		 */
//		if ((errno != EEXIST && errno != EACCES))
		if (errno != EEXIST)
			elog(ERROR, "Could not create lock file \"%s\": %s",
				 lock_file, strerror(errno));

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */

		fp_out = fopen(lock_file, "r");
		if (fp_out == NULL)
		{
			if (errno == ENOENT)
				continue; 	/* race condition; try again */
			elog(ERROR, "Cannot open lock file \"%s\": %s", lock_file, strerror(errno));
		}

		len = fread(buffer, 1, sizeof(buffer) - 1, fp_out);
		if (ferror(fp_out))
			elog(ERROR, "Cannot read from lock file: \"%s\"", lock_file);
		fclose(fp_out);

		/*
		 * There are several possible reasons for lock file
		 * to be empty:
		 * - system crash
		 * - process crash
		 * - race between writer and reader
		 *
		 * Consider empty file to be stale after LOCK_STALE_TIMEOUT attempts.
		 *
		 * TODO: alternatively we can write into temp file (lock_file_%pid),
		 * rename it and then re-read lock file to make sure,
		 * that we are successfully acquired the lock.
		 */
		if (len == 0)
		{
			if (empty_tries == 0)
			{
				elog(WARNING, "Lock file \"%s\" is empty", lock_file);
				goto grab_lock;
			}

			if ((empty_tries % LOG_FREQ) == 0)
				elog(WARNING, "Waiting %u seconds on empty exclusive lock for backup %s",
						 empty_tries, backup_id);

			sleep(1);
			/*
			 * waiting on empty lock file should not affect
			 * the timer for concurrent lockers (ntries).
			 */
			empty_tries--;
			ntries++;
			continue;
		}

		encoded_pid = atoi(buffer);

		if (encoded_pid <= 0)
		{
			elog(WARNING, "Bogus data in lock file \"%s\": \"%s\"",
				 lock_file, buffer);
			goto grab_lock;
		}

		/*
		 * Check to see if the other process still exists
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.
		 */
		if (encoded_pid == my_pid)
			return LOCK_OK;

		if (kill(encoded_pid, 0) == 0)
		{
			/* complain every fifth interval */
			if ((ntries % LOG_FREQ) == 0)
			{
				elog(WARNING, "Process %d is using backup %s, and is still running",
					 encoded_pid, backup_id);

				elog(WARNING, "Waiting %u seconds on exclusive lock for backup %s",
					 ntries, backup_id);
			}

			sleep(1);

			/* try again */
			continue;
		}
		else
		{
			if (errno == ESRCH)
				elog(WARNING, "Process %d which used backup %s no longer exists",
					 encoded_pid, backup_id);
			else
				elog(ERROR, "Failed to send signal 0 to a process %d: %s",
					encoded_pid, strerror(errno));
		}

grab_lock:
		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.  Need a loop because of possible race condition against other
		 * would-be creators.
		 */
		if (fio_unlink(lock_file, FIO_BACKUP_HOST) < 0)
		{
			if (errno == ENOENT)
				continue; /* race condition, again */
			elog(ERROR, "Could not remove old lock file \"%s\": %s",
				 lock_file, strerror(errno));
		}

	} while (ntries--);

	/* Failed to acquire exclusive lock in time */
	if (fd <= 0)
		return LOCK_FAIL_TIMEOUT;

	/*
	 * Successfully created the file, now fill it.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n", my_pid);

	errno = 0;
	if (fio_write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);

		/* In lax mode if we failed to grab lock because of 'out of space error',
		 * then treat backup as locked.
		 * Only delete command should be run in lax mode.
		 */
		if (!strict && save_errno == ENOSPC)
			return LOCK_FAIL_ENOSPC;
		else
			elog(ERROR, "Could not write lock file \"%s\": %s",
				 lock_file, strerror(save_errno));
	}

	if (fio_flush(fd) != 0)
	{
		int save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);

		/* In lax mode if we failed to grab lock because of 'out of space error',
		 * then treat backup as locked.
		 * Only delete command should be run in lax mode.
		 */
		if (!strict && save_errno == ENOSPC)
			return LOCK_FAIL_ENOSPC;
		else
			elog(ERROR, "Could not flush lock file \"%s\": %s",
					lock_file, strerror(save_errno));
	}

	if (fio_close(fd) != 0)
	{
		int save_errno = errno;

		fio_unlink(lock_file, FIO_BACKUP_HOST);

		if (!strict && errno == ENOSPC)
			return LOCK_FAIL_ENOSPC;
		else
			elog(ERROR, "Could not close lock file \"%s\": %s",
				 lock_file, strerror(save_errno));
	}

//	elog(LOG, "Acquired exclusive lock for backup %s after %ds",
//			base36enc(backup->start_time),
//			LOCK_TIMEOUT - ntries + LOCK_STALE_TIMEOUT - empty_tries);

	return LOCK_OK;
}

/* Wait until all shared lock owners are gone
 * 0 - successs
 * 1 - fail
 */
int
wait_shared_owners(pgBackup *backup)
{
    FILE *fp = NULL;
    char  buffer[256];
    pid_t encoded_pid = 0;
    int   ntries = LOCK_TIMEOUT;
    char  lock_file[MAXPGPATH];

    join_path_components(lock_file, backup->root_dir, BACKUP_RO_LOCK_FILE);

    fp = fopen(lock_file, "r");
    if (fp == NULL && errno != ENOENT)
        elog(ERROR, "Cannot open lock file \"%s\": %s", lock_file, strerror(errno));

    /* iterate over pids in lock file */
    while (fp && fgets(buffer, sizeof(buffer), fp))
    {
        encoded_pid = atoi(buffer);
        if (encoded_pid <= 0)
        {
            elog(WARNING, "Bogus data in lock file \"%s\": \"%s\"", lock_file, buffer);
            continue;
        }

        /* wait until shared lock owners go away */
        do
        {
            if (interrupted)
                elog(ERROR, "Interrupted while locking backup %s",
                    base36enc(backup->start_time));

            if (encoded_pid == my_pid)
                break;

            /* check if lock owner is still alive */
            if (kill(encoded_pid, 0) == 0)
            {
                /* complain from time to time */
                if ((ntries % LOG_FREQ) == 0)
                {
                    elog(WARNING, "Process %d is using backup %s in shared mode, and is still running",
                            encoded_pid, base36enc(backup->start_time));

                    elog(WARNING, "Waiting %u seconds on lock for backup %s", ntries,
                            base36enc(backup->start_time));
                }

                sleep(1);

                /* try again */
                continue;
            }
            else if (errno != ESRCH)
                elog(ERROR, "Failed to send signal 0 to a process %d: %s",
                        encoded_pid, strerror(errno));

            /* locker is dead */
            break;

        } while (ntries--);
    }

    if (fp && ferror(fp))
        elog(ERROR, "Cannot read from lock file: \"%s\"", lock_file);

    if (fp)
        fclose(fp);

    /* some shared owners are still alive */
    if (ntries <= 0)
    {
        elog(WARNING, "Cannot to lock backup %s in exclusive mode, because process %u owns shared lock",
                base36enc(backup->start_time), encoded_pid);
        return 1;
    }

    /* unlink shared lock file */
    fio_unlink(lock_file, FIO_BACKUP_HOST);
    return 0;
}

/*
 * Lock backup in shared mode
 * 0 - successs
 * 1 - fail
 */
int
grab_shared_lock_file(pgBackup *backup)
{
	FILE *fp_in = NULL;
	FILE *fp_out = NULL;
	char  buf_in[256];
	pid_t encoded_pid;
	char  lock_file[MAXPGPATH];

	char  buffer[8192]; /*TODO: should be enough, but maybe malloc+realloc is better ? */
	char  lock_file_tmp[MAXPGPATH];
	int   buffer_len = 0;

	join_path_components(lock_file, backup->root_dir, BACKUP_RO_LOCK_FILE);
	snprintf(lock_file_tmp, MAXPGPATH, "%s%s", lock_file, "tmp");

	/* open already existing lock files */
	fp_in = fopen(lock_file, "r");
	if (fp_in == NULL && errno != ENOENT)
		elog(ERROR, "Cannot open lock file \"%s\": %s", lock_file, strerror(errno));

	/* read PIDs of owners */
	while (fp_in && fgets(buf_in, sizeof(buf_in), fp_in))
	{
		encoded_pid = atoi(buf_in);
		if (encoded_pid <= 0)
		{
			elog(WARNING, "Bogus data in lock file \"%s\": \"%s\"", lock_file, buf_in);
			continue;
		}

		if (encoded_pid == my_pid)
			continue;

		if (kill(encoded_pid, 0) == 0)
		{
			/*
			 * Somebody is still using this backup in shared mode,
			 * copy this pid into a new file.
			 */
			buffer_len += snprintf(buffer+buffer_len, 4096, "%u\n", encoded_pid);
		}
		else if (errno != ESRCH)
			elog(ERROR, "Failed to send signal 0 to a process %d: %s",
					encoded_pid, strerror(errno));
    }

	if (fp_in)
	{
		if (ferror(fp_in))
			elog(ERROR, "Cannot read from lock file: \"%s\"", lock_file);
		fclose(fp_in);
	}

	fp_out = fopen(lock_file_tmp, "w");
	if (fp_out == NULL)
	{
		if (errno == EROFS)
			return 0;

		elog(ERROR, "Cannot open temp lock file \"%s\": %s", lock_file_tmp, strerror(errno));
	}

	/* add my own pid */
	buffer_len += snprintf(buffer+buffer_len, sizeof(buffer), "%u\n", my_pid);

	/* write out the collected PIDs to temp lock file */
	fwrite(buffer, 1, buffer_len, fp_out);

	if (ferror(fp_out))
		elog(ERROR, "Cannot write to lock file: \"%s\"", lock_file_tmp);

	if (fclose(fp_out) != 0)
		elog(ERROR, "Cannot close temp lock file \"%s\": %s", lock_file_tmp, strerror(errno));

	if (rename(lock_file_tmp, lock_file) < 0)
		elog(ERROR, "Cannot rename file \"%s\" to \"%s\": %s",
			lock_file_tmp, lock_file, strerror(errno));

	return 0;
}

void
unlock_backup(const char *backup_dir, const char *backup_id, bool exclusive)
{
	if (exclusive)
	{
		release_excl_lock_file(backup_dir);
		return;
	}

	/* To remove shared lock, we must briefly obtain exclusive lock, ... */
	if (grab_excl_lock_file(backup_dir, backup_id, false) != LOCK_OK)
		/* ... if it's not possible then leave shared lock */
		return;

	release_shared_lock_file(backup_dir);
	release_excl_lock_file(backup_dir);
}

void
release_excl_lock_file(const char *backup_dir)
{
	char  lock_file[MAXPGPATH];

	join_path_components(lock_file, backup_dir, BACKUP_LOCK_FILE);

	/* TODO Sanity check: maybe we should check, that pid in lock file is my_pid */

	/* unlink pid file */
	fio_unlink(lock_file, FIO_BACKUP_HOST);
}

void
release_shared_lock_file(const char *backup_dir)
{
	FILE *fp_in = NULL;
	FILE *fp_out = NULL;
	char  buf_in[256];
	pid_t encoded_pid;
	char  lock_file[MAXPGPATH];

	char  buffer[8192]; /*TODO: should be enough, but maybe malloc+realloc is better ? */
	char  lock_file_tmp[MAXPGPATH];
	int   buffer_len = 0;

	join_path_components(lock_file, backup_dir, BACKUP_RO_LOCK_FILE);
	snprintf(lock_file_tmp, MAXPGPATH, "%s%s", lock_file, "tmp");

	/* open lock file */
	fp_in = fopen(lock_file, "r");
	if (fp_in == NULL)
	{
		if (errno == ENOENT)
			return;
		else
			elog(ERROR, "Cannot open lock file \"%s\": %s", lock_file, strerror(errno));
	}

	/* read PIDs of owners */
	while (fgets(buf_in, sizeof(buf_in), fp_in))
	{
		encoded_pid = atoi(buf_in);

		if (encoded_pid <= 0)
		{
			elog(WARNING, "Bogus data in lock file \"%s\": \"%s\"", lock_file, buf_in);
			continue;
		}

		/* remove my pid */
		if (encoded_pid == my_pid)
			continue;

		if (kill(encoded_pid, 0) == 0)
		{
			/*
			 * Somebody is still using this backup in shared mode,
			 * copy this pid into a new file.
			 */
			buffer_len += snprintf(buffer+buffer_len, 4096, "%u\n", encoded_pid);
		}
		else if (errno != ESRCH)
			elog(ERROR, "Failed to send signal 0 to a process %d: %s",
					encoded_pid, strerror(errno));
    }

	if (ferror(fp_in))
		elog(ERROR, "Cannot read from lock file: \"%s\"", lock_file);
	fclose(fp_in);

	/* if there is no active pid left, then there is nothing to do */
	if (buffer_len == 0)
	{
		fio_unlink(lock_file, FIO_BACKUP_HOST);
		return;
	}

	fp_out = fopen(lock_file_tmp, "w");
	if (fp_out == NULL)
		elog(ERROR, "Cannot open temp lock file \"%s\": %s", lock_file_tmp, strerror(errno));

	/* write out the collected PIDs to temp lock file */
	fwrite(buffer, 1, buffer_len, fp_out);

	if (ferror(fp_out))
		elog(ERROR, "Cannot write to lock file: \"%s\"", lock_file_tmp);

	if (fclose(fp_out) != 0)
		elog(ERROR, "Cannot close temp lock file \"%s\": %s", lock_file_tmp, strerror(errno));

	if (rename(lock_file_tmp, lock_file) < 0)
		elog(ERROR, "Cannot rename file \"%s\" to \"%s\": %s",
			lock_file_tmp, lock_file, strerror(errno));

	return;
}

/*
 * Get backup_mode in string representation.
 */
const char *
pgBackupGetBackupMode(pgBackup *backup)
{
	return backupModes[backup->backup_mode];
}

static bool
IsDir(const char *dirpath, const char *entry, fio_location location)
{
	char		path[MAXPGPATH];
	struct stat	st;

	join_path_components(path, dirpath, entry);

	return fio_stat(path, &st, false, location) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Create list of instances in given backup catalog.
 *
 * Returns parray of "InstanceConfig" structures, filled with
 * actual config of each instance.
 */
parray *
catalog_get_instance_list(void)
{
	char		path[MAXPGPATH];
	DIR		   *dir;
	struct dirent *dent;
	parray		*instances;

	instances = parray_new();

	/* open directory and list contents */
	join_path_components(path, backup_path, BACKUPS_DIR);
	dir = opendir(path);
	if (dir == NULL)
		elog(ERROR, "Cannot open directory \"%s\": %s",
			 path, strerror(errno));

	while (errno = 0, (dent = readdir(dir)) != NULL)
	{
		char		child[MAXPGPATH];
		struct stat	st;
		InstanceConfig *instance;

		/* skip entries point current dir or parent dir */
		if (strcmp(dent->d_name, ".") == 0 ||
			strcmp(dent->d_name, "..") == 0)
			continue;

		join_path_components(child, path, dent->d_name);

		if (lstat(child, &st) == -1)
			elog(ERROR, "Cannot stat file \"%s\": %s",
					child, strerror(errno));

		if (!S_ISDIR(st.st_mode))
			continue;

		instance = readInstanceConfigFile(dent->d_name);

		parray_append(instances, instance);
	}

	/* TODO 3.0: switch to ERROR */
	if (parray_num(instances) == 0)
		elog(WARNING, "This backup catalog contains no backup instances. Backup instance can be added via 'add-instance' command.");

	if (errno)
		elog(ERROR, "Cannot read directory \"%s\": %s",
				path, strerror(errno));

	if (closedir(dir))
		elog(ERROR, "Cannot close directory \"%s\": %s",
				path, strerror(errno));

	return instances;
}

/*
 * Create list of backups.
 * If 'requested_backup_id' is INVALID_BACKUP_ID, return list of all backups.
 * The list is sorted in order of descending start time.
 * If valid backup id is passed only matching backup will be added to the list.
 */
parray *
catalog_get_backup_list(const char *instance_name, time_t requested_backup_id)
{
	DIR		   *data_dir = NULL;
	struct dirent *data_ent = NULL;
	parray	   *backups = NULL;
	int			i;
	char backup_instance_path[MAXPGPATH];

	sprintf(backup_instance_path, "%s/%s/%s",
			backup_path, BACKUPS_DIR, instance_name);

	/* open backup instance backups directory */
	data_dir = fio_opendir(backup_instance_path, FIO_BACKUP_HOST);
	if (data_dir == NULL)
	{
		elog(WARNING, "cannot open directory \"%s\": %s", backup_instance_path,
			strerror(errno));
		goto err_proc;
	}

	/* scan the directory and list backups */
	backups = parray_new();
	for (; (data_ent = fio_readdir(data_dir)) != NULL; errno = 0)
	{
		char		backup_conf_path[MAXPGPATH];
		char		data_path[MAXPGPATH];
		pgBackup   *backup = NULL;

		/* skip not-directory entries and hidden entries */
		if (!IsDir(backup_instance_path, data_ent->d_name, FIO_BACKUP_HOST)
			|| data_ent->d_name[0] == '.')
			continue;

		/* open subdirectory of specific backup */
		join_path_components(data_path, backup_instance_path, data_ent->d_name);

		/* read backup information from BACKUP_CONTROL_FILE */
		join_path_components(backup_conf_path, data_path, BACKUP_CONTROL_FILE);
		backup = readBackupControlFile(backup_conf_path);

		if (!backup)
		{
			backup = pgut_new(pgBackup);
			pgBackupInit(backup);
			backup->start_time = base36dec(data_ent->d_name);
		}
		else if (strcmp(base36enc(backup->start_time), data_ent->d_name) != 0)
		{
			elog(WARNING, "backup ID in control file \"%s\" doesn't match name of the backup folder \"%s\"",
				 base36enc(backup->start_time), backup_conf_path);
		}

		backup->root_dir = pgut_strdup(data_path);

		backup->database_dir = pgut_malloc(MAXPGPATH);
		join_path_components(backup->database_dir, backup->root_dir, DATABASE_DIR);

		/* Initialize page header map */
		init_header_map(backup);

		/* TODO: save encoded backup id */
		backup->backup_id = backup->start_time;
		if (requested_backup_id != INVALID_BACKUP_ID
			&& requested_backup_id != backup->start_time)
		{
			pgBackupFree(backup);
			continue;
		}
		parray_append(backups, backup);
	}

	if (errno)
	{
		elog(WARNING, "Cannot read backup root directory \"%s\": %s",
			backup_instance_path, strerror(errno));
		goto err_proc;
	}

	fio_closedir(data_dir);
	data_dir = NULL;

	parray_qsort(backups, pgBackupCompareIdDesc);

	/* Link incremental backups with their ancestors.*/
	for (i = 0; i < parray_num(backups); i++)
	{
		pgBackup   *curr = parray_get(backups, i);
		pgBackup  **ancestor;
		pgBackup	key;

		if (curr->backup_mode == BACKUP_MODE_FULL)
			continue;

		key.start_time = curr->parent_backup;
		ancestor = (pgBackup **) parray_bsearch(backups, &key,
												pgBackupCompareIdDesc);
		if (ancestor)
			curr->parent_backup_link = *ancestor;
	}

	return backups;

err_proc:
	if (data_dir)
		fio_closedir(data_dir);
	if (backups)
		parray_walk(backups, pgBackupFree);
	parray_free(backups);

	elog(ERROR, "Failed to get backup list");

	return NULL;
}

/*
 * Create list of backup datafiles.
 * If 'requested_backup_id' is INVALID_BACKUP_ID, exit with error.
 * If valid backup id is passed only matching backup will be added to the list.
 * TODO this function only used once. Is it really needed?
 */
parray *
get_backup_filelist(pgBackup *backup, bool strict)
{
	parray		*files = NULL;
	char		backup_filelist_path[MAXPGPATH];

	join_path_components(backup_filelist_path, backup->root_dir, DATABASE_FILE_LIST);
	files = dir_read_file_list(NULL, NULL, backup_filelist_path, FIO_BACKUP_HOST, backup->content_crc);

	/* redundant sanity? */
	if (!files)
		elog(strict ? ERROR : WARNING, "Failed to get file list for backup %s", base36enc(backup->start_time));

	return files;
}

/*
 * Lock list of backups. Function goes in backward direction.
 */
void
catalog_lock_backup_list(parray *backup_list, int from_idx, int to_idx, bool strict, bool exclusive)
{
	int			start_idx,
				end_idx;
	int			i;

	if (parray_num(backup_list) == 0)
		return;

	start_idx = Max(from_idx, to_idx);
	end_idx = Min(from_idx, to_idx);

	for (i = start_idx; i >= end_idx; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);
		if (!lock_backup(backup, strict, exclusive))
			elog(ERROR, "Cannot lock backup %s directory",
				 base36enc(backup->start_time));
	}
}

/*
 * Find the latest valid child of latest valid FULL backup on given timeline
 */
pgBackup *
catalog_get_last_data_backup(parray *backup_list, TimeLineID tli, time_t current_start_time)
{
	int			i;
	pgBackup   *full_backup = NULL;
	pgBackup   *tmp_backup = NULL;
	char 	   *invalid_backup_id;

	/* backup_list is sorted in order of descending ID */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *) parray_get(backup_list, i);

		if ((backup->backup_mode == BACKUP_MODE_FULL &&
			(backup->status == BACKUP_STATUS_OK ||
			 backup->status == BACKUP_STATUS_DONE)) && backup->tli == tli)
		{
			full_backup = backup;
			break;
		}
	}

	/* Failed to find valid FULL backup to fulfill ancestor role */
	if (!full_backup)
		return NULL;

	elog(LOG, "Latest valid FULL backup: %s",
		base36enc(full_backup->start_time));

	/* FULL backup is found, lets find his latest child */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *) parray_get(backup_list, i);

		/* only valid descendants are acceptable for evaluation */
		if ((backup->status == BACKUP_STATUS_OK ||
			backup->status == BACKUP_STATUS_DONE))
		{
			switch (scan_parent_chain(backup, &tmp_backup))
			{
				/* broken chain */
				case ChainIsBroken:
					invalid_backup_id = base36enc_dup(tmp_backup->parent_backup);

					elog(WARNING, "Backup %s has missing parent: %s. Cannot be a parent",
						base36enc(backup->start_time), invalid_backup_id);
					pg_free(invalid_backup_id);
					continue;

				/* chain is intact, but at least one parent is invalid */
				case ChainIsInvalid:
					invalid_backup_id = base36enc_dup(tmp_backup->start_time);

					elog(WARNING, "Backup %s has invalid parent: %s. Cannot be a parent",
						base36enc(backup->start_time), invalid_backup_id);
					pg_free(invalid_backup_id);
					continue;

				/* chain is ok */
				case ChainIsOk:
					/* Yes, we could call is_parent() earlier - after choosing the ancestor,
					 * but this way we have an opportunity to detect and report all possible
					 * anomalies.
					 */
					if (is_parent(full_backup->start_time, backup, true))
						return backup;
			}
		}
		/* skip yourself */
		else if (backup->start_time == current_start_time)
			continue;
		else
		{
			elog(WARNING, "Backup %s has status: %s. Cannot be a parent.",
				base36enc(backup->start_time), status2str(backup->status));
		}
	}

	return NULL;
}

/*
 * For multi-timeline chain, look up suitable parent for incremental backup.
 * Multi-timeline chain has full backup and one or more descendants located
 * on different timelines.
 */
pgBackup *
get_multi_timeline_parent(parray *backup_list, parray *tli_list,
	                      TimeLineID current_tli, time_t current_start_time,
						  InstanceConfig *instance)
{
	int           i;
	timelineInfo *my_tlinfo = NULL;
	timelineInfo *tmp_tlinfo = NULL;
	pgBackup     *ancestor_backup = NULL;

	/* there are no timelines in the archive */
	if (parray_num(tli_list) == 0)
		return NULL;

	/* look for current timelineInfo */
	for (i = 0; i < parray_num(tli_list); i++)
	{
		timelineInfo  *tlinfo = (timelineInfo  *) parray_get(tli_list, i);

		if (tlinfo->tli == current_tli)
		{
			my_tlinfo = tlinfo;
			break;
		}
	}

	if (my_tlinfo == NULL)
		return NULL;

	/* Locate tlinfo of suitable full backup.
	 * Consider this example:
	 *  t3                    s2-------X <-! We are here
	 *                        /
	 *  t2         s1----D---*----E--->
	 *             /
	 *  t1--A--B--*---C------->
	 *
	 * A, E - full backups
	 * B, C, D - incremental backups
	 *
	 * We must find A.
	 */
	tmp_tlinfo = my_tlinfo;
	while (tmp_tlinfo->parent_link)
	{
		/* if timeline has backups, iterate over them */
		if (tmp_tlinfo->parent_link->backups)
		{
			for (i = 0; i < parray_num(tmp_tlinfo->parent_link->backups); i++)
			{
				pgBackup *backup = (pgBackup *) parray_get(tmp_tlinfo->parent_link->backups, i);

				if (backup->backup_mode == BACKUP_MODE_FULL &&
					(backup->status == BACKUP_STATUS_OK ||
					 backup->status == BACKUP_STATUS_DONE) &&
					 backup->stop_lsn <= tmp_tlinfo->switchpoint)
				{
					ancestor_backup = backup;
					break;
				}
			}
		}

		if (ancestor_backup)
			break;

		tmp_tlinfo = tmp_tlinfo->parent_link;
	}

	/* failed to find valid FULL backup on parent timelines */
	if (!ancestor_backup)
		return NULL;
	else
		elog(LOG, "Latest valid full backup: %s, tli: %i",
			base36enc(ancestor_backup->start_time), ancestor_backup->tli);

	/* At this point we found suitable full backup,
	 * now we must find his latest child, suitable to be
	 * parent of current incremental backup.
	 * Consider this example:
	 *  t3                    s2-------X <-! We are here
	 *                        /
	 *  t2         s1----D---*----E--->
	 *             /
	 *  t1--A--B--*---C------->
	 *
	 * A, E - full backups
	 * B, C, D - incremental backups
	 *
	 * We found A, now we must find D.
	 */

	/* Optimistically, look on current timeline for valid incremental backup, child of ancestor */
	if (my_tlinfo->backups)
	{
		/* backups are sorted in descending order and we need latest valid */
		for (i = 0; i < parray_num(my_tlinfo->backups); i++)
		{
			pgBackup *tmp_backup = NULL;
			pgBackup *backup = (pgBackup *) parray_get(my_tlinfo->backups, i);

			/* found suitable parent */
			if (scan_parent_chain(backup, &tmp_backup) == ChainIsOk &&
				is_parent(ancestor_backup->start_time, backup, false))
				return backup;
		}
	}

	/* Iterate over parent timelines and look for a valid backup, child of ancestor */
	tmp_tlinfo = my_tlinfo;
	while (tmp_tlinfo->parent_link)
	{

		/* if timeline has backups, iterate over them */
		if (tmp_tlinfo->parent_link->backups)
		{
			for (i = 0; i < parray_num(tmp_tlinfo->parent_link->backups); i++)
			{
				pgBackup *tmp_backup = NULL;
				pgBackup *backup = (pgBackup *) parray_get(tmp_tlinfo->parent_link->backups, i);

				/* We are not interested in backups
				 * located outside of our timeline history
				 */
				if (backup->stop_lsn > tmp_tlinfo->switchpoint)
					continue;

				if (scan_parent_chain(backup, &tmp_backup) == ChainIsOk &&
					is_parent(ancestor_backup->start_time, backup, true))
					return backup;
			}
		}

		tmp_tlinfo = tmp_tlinfo->parent_link;
	}

	return NULL;
}

/* Create backup directory in $BACKUP_PATH
 * Note, that backup_id attribute is updated,
 * so it is possible to get diffrent values in
 * pgBackup.start_time and pgBackup.backup_id.
 * It may be ok or maybe not, so it's up to the caller
 * to fix it or let it be.
 */

void
pgBackupCreateDir(pgBackup *backup, const char *backup_instance_path)
{
	int		i;
	parray *subdirs = parray_new();

	parray_append(subdirs, pg_strdup(DATABASE_DIR));

	/* Add external dirs containers */
	if (backup->external_dir_str)
	{
		parray *external_list;

		external_list = make_external_directory_list(backup->external_dir_str,
													 false);
		for (i = 0; i < parray_num(external_list); i++)
		{
			char		temp[MAXPGPATH];
			/* Numeration of externaldirs starts with 1 */
			makeExternalDirPathByNum(temp, EXTERNAL_DIR, i+1);
			parray_append(subdirs, pg_strdup(temp));
		}
		free_dir_list(external_list);
	}

	backup->backup_id = create_backup_dir(backup, backup_instance_path);

	if (backup->backup_id == 0)
		elog(ERROR, "Cannot create backup directory: %s", strerror(errno));

	backup->database_dir = pgut_malloc(MAXPGPATH);
	join_path_components(backup->database_dir, backup->root_dir, DATABASE_DIR);

	/* block header map */
	init_header_map(backup);

	/* create directories for actual backup files */
	for (i = 0; i < parray_num(subdirs); i++)
	{
		char	path[MAXPGPATH];

		join_path_components(path, backup->root_dir, parray_get(subdirs, i));
		fio_mkdir(path, DIR_PERMISSION, FIO_BACKUP_HOST);
	}

	free_dir_list(subdirs);
}

/*
 * Create root directory for backup,
 * update pgBackup.root_dir if directory creation was a success
 */
time_t
create_backup_dir(pgBackup *backup, const char *backup_instance_path)
{
	int     attempts = 10;

	while (attempts--)
	{
		int    rc;
		char   path[MAXPGPATH];
		time_t backup_id = time(NULL);

		join_path_components(path, backup_instance_path, base36enc(backup_id));

		/* TODO: add wrapper for remote mode */
		rc = dir_create_dir(path, DIR_PERMISSION, true);

		if (rc == 0)
		{
			backup->root_dir = pgut_strdup(path);
			return backup_id;
		}
		else
		{
			elog(WARNING, "Cannot create directory \"%s\": %s", path, strerror(errno));
			sleep(1);
		}
	}

	return 0;
}

/*
 * Create list of timelines.
 * TODO: '.partial' and '.part' segno information should be added to tlinfo.
 */
parray *
catalog_get_timelines(InstanceConfig *instance)
{
	int i,j,k;
	parray *xlog_files_list = parray_new();
	parray *timelineinfos;
	parray *backups;
	timelineInfo *tlinfo;
	char		arclog_path[MAXPGPATH];

	/* for fancy reporting */
	char begin_segno_str[MAXFNAMELEN];
	char end_segno_str[MAXFNAMELEN];

	/* read all xlog files that belong to this archive */
	sprintf(arclog_path, "%s/%s/%s", backup_path, "wal", instance->name);
	dir_list_file(xlog_files_list, arclog_path, false, true, false, false, true, 0, FIO_BACKUP_HOST);
	parray_qsort(xlog_files_list, pgFileCompareName);

	timelineinfos = parray_new();
	tlinfo = NULL;

	/* walk through files and collect info about timelines */
	for (i = 0; i < parray_num(xlog_files_list); i++)
	{
		pgFile *file = (pgFile *) parray_get(xlog_files_list, i);
		TimeLineID tli;
		parray *timelines;
		xlogFile *wal_file = NULL;

		/*
		 * Regular WAL file.
		 * IsXLogFileName() cannot be used here
		 */
		if (strspn(file->name, "0123456789ABCDEF") == XLOG_FNAME_LEN)
		{
			int result = 0;
			uint32 log, seg;
			XLogSegNo segno = 0;
			char suffix[MAXFNAMELEN];

			result = sscanf(file->name, "%08X%08X%08X.%s",
						&tli, &log, &seg, (char *) &suffix);

			/* sanity */
			if (result < 3)
			{
				elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
				continue;
			}

			/* get segno from log */
			GetXLogSegNoFromScrath(segno, log, seg, instance->xlog_seg_size);

			/* regular WAL file with suffix */
			if (result == 4)
			{
				/* backup history file. Currently we don't use them */
				if (IsBackupHistoryFileName(file->name))
				{
					elog(VERBOSE, "backup history file \"%s\"", file->name);

					if (!tlinfo || tlinfo->tli != tli)
					{
						tlinfo = timelineInfoNew(tli);
						parray_append(timelineinfos, tlinfo);
					}

					/* append file to xlog file list */
					wal_file = palloc(sizeof(xlogFile));
					wal_file->file = *file;
					wal_file->segno = segno;
					wal_file->type = BACKUP_HISTORY_FILE;
					wal_file->keep = false;
					parray_append(tlinfo->xlog_filelist, wal_file);
					continue;
				}
				/* partial WAL segment */
				else if (IsPartialXLogFileName(file->name) ||
						 IsPartialCompressXLogFileName(file->name))
				{
					elog(VERBOSE, "partial WAL file \"%s\"", file->name);

					if (!tlinfo || tlinfo->tli != tli)
					{
						tlinfo = timelineInfoNew(tli);
						parray_append(timelineinfos, tlinfo);
					}

					/* append file to xlog file list */
					wal_file = palloc(sizeof(xlogFile));
					wal_file->file = *file;
					wal_file->segno = segno;
					wal_file->type = PARTIAL_SEGMENT;
					wal_file->keep = false;
					parray_append(tlinfo->xlog_filelist, wal_file);
					continue;
				}
				/* temp WAL segment */
				else if (IsTempXLogFileName(file->name) ||
						 IsTempCompressXLogFileName(file->name))
				{
					elog(VERBOSE, "temp WAL file \"%s\"", file->name);

					if (!tlinfo || tlinfo->tli != tli)
					{
						tlinfo = timelineInfoNew(tli);
						parray_append(timelineinfos, tlinfo);
					}

					/* append file to xlog file list */
					wal_file = palloc(sizeof(xlogFile));
					wal_file->file = *file;
					wal_file->segno = segno;
					wal_file->type = TEMP_SEGMENT;
					wal_file->keep = false;
					parray_append(tlinfo->xlog_filelist, wal_file);
					continue;
				}
				/* we only expect compressed wal files with .gz suffix */
				else if (strcmp(suffix, "gz") != 0)
				{
					elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
					continue;
				}
			}

			/* new file belongs to new timeline */
			if (!tlinfo || tlinfo->tli != tli)
			{
				tlinfo = timelineInfoNew(tli);
				parray_append(timelineinfos, tlinfo);
			}
			/*
			 * As it is impossible to detect if segments before segno are lost,
			 * or just do not exist, do not report them as lost.
			 */
			else if (tlinfo->n_xlog_files != 0)
			{
				/* check, if segments are consequent */
				XLogSegNo expected_segno = tlinfo->end_segno + 1;

				/*
				 * Some segments are missing. remember them in lost_segments to report.
				 * Normally we expect that segment numbers form an increasing sequence,
				 * though it's legal to find two files with equal segno in case there
				 * are both compressed and non-compessed versions. For example
				 * 000000010000000000000002 and 000000010000000000000002.gz
				 *
				 */
				if (segno != expected_segno && segno != tlinfo->end_segno)
				{
					xlogInterval *interval = palloc(sizeof(xlogInterval));;
					interval->begin_segno = expected_segno;
					interval->end_segno = segno - 1;

					if (tlinfo->lost_segments == NULL)
						tlinfo->lost_segments = parray_new();

					parray_append(tlinfo->lost_segments, interval);
				}
			}

			if (tlinfo->begin_segno == 0)
				tlinfo->begin_segno = segno;

			/* this file is the last for this timeline so far */
			tlinfo->end_segno = segno;
			/* update counters */
			tlinfo->n_xlog_files++;
			tlinfo->size += file->size;

			/* append file to xlog file list */
			wal_file = palloc(sizeof(xlogFile));
			wal_file->file = *file;
			wal_file->segno = segno;
			wal_file->type = SEGMENT;
			wal_file->keep = false;
			parray_append(tlinfo->xlog_filelist, wal_file);
		}
		/* timeline history file */
		else if (IsTLHistoryFileName(file->name))
		{
			TimeLineHistoryEntry *tln;

			sscanf(file->name, "%08X.history", &tli);
			timelines = read_timeline_history(arclog_path, tli, true);

			/* History file is empty or corrupted, disregard it */
			if (!timelines)
				continue;

			if (!tlinfo || tlinfo->tli != tli)
			{
				tlinfo = timelineInfoNew(tli);
				parray_append(timelineinfos, tlinfo);
				/*
				 * 1 is the latest timeline in the timelines list.
				 * 0 - is our timeline, which is of no interest here
				 */
				tln = (TimeLineHistoryEntry *) parray_get(timelines, 1);
				tlinfo->switchpoint = tln->end;
				tlinfo->parent_tli = tln->tli;

				/* find parent timeline to link it with this one */
				for (j = 0; j < parray_num(timelineinfos); j++)
				{
					timelineInfo *cur = (timelineInfo *) parray_get(timelineinfos, j);
					if (cur->tli == tlinfo->parent_tli)
					{
						tlinfo->parent_link = cur;
						break;
					}
				}
			}

			parray_walk(timelines, pfree);
			parray_free(timelines);
		}
		else
			elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
	}

	/* save information about backups belonging to each timeline */
	backups = catalog_get_backup_list(instance->name, INVALID_BACKUP_ID);

	for (i = 0; i < parray_num(timelineinfos); i++)
	{
		timelineInfo *tlinfo = parray_get(timelineinfos, i);
		for (j = 0; j < parray_num(backups); j++)
		{
			pgBackup *backup = parray_get(backups, j);
			if (tlinfo->tli == backup->tli)
			{
				if (tlinfo->backups == NULL)
					tlinfo->backups = parray_new();

				parray_append(tlinfo->backups, backup);
			}
		}
	}

	/* determine oldest backup and closest backup for every timeline */
	for (i = 0; i < parray_num(timelineinfos); i++)
	{
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		tlinfo->oldest_backup = get_oldest_backup(tlinfo);
		tlinfo->closest_backup = get_closest_backup(tlinfo);
	}

	/* determine which WAL segments must be kept because of wal retention */
	if (instance->wal_depth <= 0)
		return timelineinfos;

	/*
	 * WAL retention for now is fairly simple.
	 * User can set only one parameter - 'wal-depth'.
	 * It determines how many latest valid(!) backups on timeline
	 * must have an ability to perform PITR:
	 * Consider the example:
	 *
	 * ---B1-------B2-------B3-------B4--------> WAL timeline1
	 *
	 * If 'wal-depth' is set to 2, then WAL purge should produce the following result:
	 *
	 *    B1       B2       B3-------B4--------> WAL timeline1
	 *
	 * Only valid backup can satisfy 'wal-depth' condition, so if B3 is not OK or DONE,
	 * then WAL purge should produce the following result:
	 *    B1       B2-------B3-------B4--------> WAL timeline1
	 *
	 * Complicated cases, such as branched timelines are taken into account.
	 * wal-depth is applied to each timeline independently:
	 *
	 *         |--------->                       WAL timeline2
	 * ---B1---|---B2-------B3-------B4--------> WAL timeline1
	 *
	 * after WAL purge with wal-depth=2:
	 *
	 *         |--------->                       WAL timeline2
	 *    B1---|   B2       B3-------B4--------> WAL timeline1
	 *
	 * In this example WAL retention prevents purge of WAL required by tli2
	 * to stay reachable from backup B on tli1.
	 *
	 * To protect WAL from purge we try to set 'anchor_lsn' and 'anchor_tli' in every timeline.
	 * They are usually comes from 'start-lsn' and 'tli' attributes of backup
	 * calculated by 'wal-depth' parameter.
	 * With 'wal-depth=2' anchor_backup in tli1 is B3.

	 * If timeline has not enough valid backups to satisfy 'wal-depth' condition,
	 * then 'anchor_lsn' and 'anchor_tli' taken from from 'start-lsn' and 'tli
	 * attribute of closest_backup.
	 * The interval of WAL starting from closest_backup to switchpoint is
	 * saved into 'keep_segments' attribute.
	 * If there is several intermediate timelines between timeline and its closest_backup
	 * then on every intermediate timeline WAL interval between switchpoint
	 * and starting segment is placed in 'keep_segments' attributes:
	 *
	 *                |--------->                       WAL timeline3
	 *         |------|                 B5-----B6-->    WAL timeline2
	 *    B1---|   B2       B3-------B4------------>    WAL timeline1
	 *
	 * On timeline where closest_backup is located the WAL interval between
	 * closest_backup and switchpoint is placed into 'keep_segments'.
	 * If timeline has no 'closest_backup', then 'wal-depth' rules cannot be applied
	 * to this timeline and its WAL must be purged by following the basic rules of WAL purging.
	 *
	 * Third part is handling of ARCHIVE backups.
	 * If B1 and B2 have ARCHIVE wal-mode, then we must preserve WAL intervals
	 * between start_lsn and stop_lsn for each of them in 'keep_segments'.
	 */

	/* determine anchor_lsn and keep_segments for every timeline */
	for (i = 0; i < parray_num(timelineinfos); i++)
	{
		int count = 0;
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		/*
		 * Iterate backward on backups belonging to this timeline to find
		 * anchor_backup. NOTE Here we rely on the fact that backups list
		 * is ordered by start_lsn DESC.
		 */
		if (tlinfo->backups)
		{
			for (j = 0; j < parray_num(tlinfo->backups); j++)
			{
				pgBackup *backup = parray_get(tlinfo->backups, j);

				/* sanity */
				if (XLogRecPtrIsInvalid(backup->start_lsn) ||
					backup->tli <= 0)
					continue;

				/* skip invalid backups */
				if (backup->status != BACKUP_STATUS_OK &&
					backup->status != BACKUP_STATUS_DONE)
					continue;

				/*
				 * Pinned backups should be ignored for the
				 * purpose of retention fulfillment, so skip them.
				 */
				if (backup->expire_time > 0 &&
					backup->expire_time > current_time)
				{
					elog(LOG, "Pinned backup %s is ignored for the "
							"purpose of WAL retention",
						base36enc(backup->start_time));
					continue;
				}

				count++;

				if (count == instance->wal_depth)
				{
					elog(LOG, "On timeline %i WAL is protected from purge at %X/%X",
						 tlinfo->tli,
						 (uint32) (backup->start_lsn >> 32),
						 (uint32) (backup->start_lsn));

					tlinfo->anchor_lsn = backup->start_lsn;
					tlinfo->anchor_tli = backup->tli;
					break;
				}
			}
		}

		/*
		 * Failed to find anchor backup for this timeline.
		 * We cannot just thrown it to the wolves, because by
		 * doing that we will violate our own guarantees.
		 * So check the existence of closest_backup for
		 * this timeline. If there is one, then
		 * set the 'anchor_lsn' and 'anchor_tli' to closest_backup
		 * 'start-lsn' and 'tli' respectively.
		 *                      |-------------B5----------> WAL timeline3
		 *                |-----|-------------------------> WAL timeline2
		 *     B1    B2---|        B3     B4-------B6-----> WAL timeline1
		 *
		 * wal-depth=2
		 *
		 * If number of valid backups on timelines is less than 'wal-depth'
		 * then timeline must(!) stay reachable via parent timelines if any.
		 * If closest_backup is not available, then general WAL purge rules
		 * are applied.
		 */
		if (XLogRecPtrIsInvalid(tlinfo->anchor_lsn))
		{
			/*
			 * Failed to find anchor_lsn in our own timeline.
			 * Consider the case:
			 * -------------------------------------> tli5
			 * ----------------------------B4-------> tli4
			 *                     S3`--------------> tli3
			 *      S1`------------S3---B3-------B6-> tli2
			 * B1---S1-------------B2--------B5-----> tli1
			 *
			 * B* - backups
			 * S* - switchpoints
			 * wal-depth=2
			 *
			 * Expected result:
			 *            TLI5 will be purged entirely
			 *                             B4-------> tli4
			 *                     S2`--------------> tli3
			 *      S1`------------S2   B3-------B6-> tli2
			 * B1---S1             B2--------B5-----> tli1
			 */
			pgBackup *closest_backup = NULL;
			xlogInterval *interval = NULL;
			TimeLineID tli = 0;
			/* check if tli has closest_backup */
			if (!tlinfo->closest_backup)
				/* timeline has no closest_backup, wal retention cannot be
				 * applied to this timeline.
				 * Timeline will be purged up to oldest_backup if any or
				 * purge entirely if there is none.
				 * In example above: tli5 and tli4.
				 */
				continue;

			/* sanity for closest_backup */
			if (XLogRecPtrIsInvalid(tlinfo->closest_backup->start_lsn) ||
				tlinfo->closest_backup->tli <= 0)
				continue;

			/*
			 * Set anchor_lsn and anchor_tli to protect whole timeline from purge
			 * In the example above: tli3.
			 */
			tlinfo->anchor_lsn = tlinfo->closest_backup->start_lsn;
			tlinfo->anchor_tli = tlinfo->closest_backup->tli;

			/* closest backup may be located not in parent timeline */
			closest_backup = tlinfo->closest_backup;

			tli = tlinfo->tli;

			/*
			 * Iterate over parent timeline chain and
			 * look for timeline where closest_backup belong
			 */
			while (tlinfo->parent_link)
			{
				/* In case of intermediate timeline save to keep_segments
				 * begin_segno and switchpoint segment.
				 * In case of final timelines save to keep_segments
				 * closest_backup start_lsn segment and switchpoint segment.
				 */
				XLogRecPtr switchpoint = tlinfo->switchpoint;

				tlinfo = tlinfo->parent_link;

				if (tlinfo->keep_segments == NULL)
					tlinfo->keep_segments = parray_new();

				/* in any case, switchpoint segment must be added to interval */
				interval = palloc(sizeof(xlogInterval));
				GetXLogSegNo(switchpoint, interval->end_segno, instance->xlog_seg_size);

				/* Save [S1`, S2] to keep_segments */
				if (tlinfo->tli != closest_backup->tli)
					interval->begin_segno = tlinfo->begin_segno;
				/* Save [B1, S1] to keep_segments */
				else
					GetXLogSegNo(closest_backup->start_lsn, interval->begin_segno, instance->xlog_seg_size);

				/*
				 * TODO: check, maybe this interval is already here or
				 * covered by other larger interval.
				 */

				GetXLogFileName(begin_segno_str, tlinfo->tli, interval->begin_segno, instance->xlog_seg_size);
				GetXLogFileName(end_segno_str, tlinfo->tli, interval->end_segno, instance->xlog_seg_size);

				elog(LOG, "Timeline %i to stay reachable from timeline %i "
								"protect from purge WAL interval between "
								"%s and %s on timeline %i",
						tli, closest_backup->tli, begin_segno_str,
						end_segno_str, tlinfo->tli);

				parray_append(tlinfo->keep_segments, interval);
				continue;
			}
			continue;
		}

		/* Iterate over backups left */
		for (j = count; j < parray_num(tlinfo->backups); j++)
		{
			XLogSegNo   segno = 0;
			xlogInterval *interval = NULL;
			pgBackup *backup = parray_get(tlinfo->backups, j);

			/*
			 * We must calculate keep_segments intervals for ARCHIVE backups
			 * with start_lsn less than anchor_lsn.
			 */

			/* STREAM backups cannot contribute to keep_segments */
			if (backup->stream)
				continue;

			/* sanity */
			if (XLogRecPtrIsInvalid(backup->start_lsn) ||
				backup->tli <= 0)
				continue;

			/* no point in clogging keep_segments by backups protected by anchor_lsn */
			if (backup->start_lsn >= tlinfo->anchor_lsn)
				continue;

			/* append interval to keep_segments */
			interval = palloc(sizeof(xlogInterval));
			GetXLogSegNo(backup->start_lsn, segno, instance->xlog_seg_size);
			interval->begin_segno = segno;
			GetXLogSegNo(backup->stop_lsn, segno, instance->xlog_seg_size);

			/*
			 * On replica it is possible to get STOP_LSN pointing to contrecord,
			 * so set end_segno to the next segment after STOP_LSN just to be safe.
			 */
			if (backup->from_replica)
				interval->end_segno = segno + 1;
			else
				interval->end_segno = segno;

			GetXLogFileName(begin_segno_str, tlinfo->tli, interval->begin_segno, instance->xlog_seg_size);
			GetXLogFileName(end_segno_str, tlinfo->tli, interval->end_segno, instance->xlog_seg_size);

			elog(LOG, "Archive backup %s to stay consistent "
							"protect from purge WAL interval "
							"between %s and %s on timeline %i",
						base36enc(backup->start_time),
						begin_segno_str, end_segno_str, backup->tli);

			if (tlinfo->keep_segments == NULL)
				tlinfo->keep_segments = parray_new();

			parray_append(tlinfo->keep_segments, interval);
		}
	}

	/*
	 * Protect WAL segments from deletion by setting 'keep' flag.
	 * We must keep all WAL segments after anchor_lsn (including), and also segments
	 * required by ARCHIVE backups for consistency - WAL between [start_lsn, stop_lsn].
	 */
	for (i = 0; i < parray_num(timelineinfos); i++)
	{
		XLogSegNo   anchor_segno = 0;
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		/*
		 * At this point invalid anchor_lsn can be only in one case:
		 * timeline is going to be purged by regular WAL purge rules.
		 */
		if (XLogRecPtrIsInvalid(tlinfo->anchor_lsn))
			continue;

		/*
		 * anchor_lsn is located in another timeline, it means that the timeline
		 * will be protected from purge entirely.
		 */
		if (tlinfo->anchor_tli > 0 && tlinfo->anchor_tli != tlinfo->tli)
			continue;

		GetXLogSegNo(tlinfo->anchor_lsn, anchor_segno, instance->xlog_seg_size);

		for (j = 0; j < parray_num(tlinfo->xlog_filelist); j++)
		{
			xlogFile *wal_file = (xlogFile *) parray_get(tlinfo->xlog_filelist, j);

			if (wal_file->segno >= anchor_segno)
			{
				wal_file->keep = true;
				continue;
			}

			/* no keep segments */
			if (!tlinfo->keep_segments)
				continue;

			/* Protect segments belonging to one of the keep invervals */
			for (k = 0; k < parray_num(tlinfo->keep_segments); k++)
			{
				xlogInterval *keep_segments = (xlogInterval *) parray_get(tlinfo->keep_segments, k);

				if ((wal_file->segno >= keep_segments->begin_segno) &&
					wal_file->segno <= keep_segments->end_segno)
				{
					wal_file->keep = true;
					break;
				}
			}
		}
	}

	return timelineinfos;
}

/*
 * Iterate over parent timelines and look for valid backup
 * closest to given timeline switchpoint.
 *
 * If such backup doesn't exist, it means that
 * timeline is unreachable. Return NULL.
 */
pgBackup*
get_closest_backup(timelineInfo *tlinfo)
{
	pgBackup *closest_backup = NULL;
	int i;

	/*
	 * Iterate over backups belonging to parent timelines
	 * and look for candidates.
	 */
	while (tlinfo->parent_link && !closest_backup)
	{
		parray *backup_list = tlinfo->parent_link->backups;
		if (backup_list != NULL)
		{
			for (i = 0; i < parray_num(backup_list); i++)
			{
				pgBackup   *backup = parray_get(backup_list, i);

				/*
				 * Only valid backups made before switchpoint
				 * should be considered.
				 */
				if (!XLogRecPtrIsInvalid(backup->stop_lsn) &&
					XRecOffIsValid(backup->stop_lsn) &&
					backup->stop_lsn <= tlinfo->switchpoint &&
					(backup->status == BACKUP_STATUS_OK ||
					backup->status == BACKUP_STATUS_DONE))
				{
					/* Check if backup is closer to switchpoint than current candidate */
					if (!closest_backup || backup->stop_lsn > closest_backup->stop_lsn)
						closest_backup = backup;
				}
			}
		}

		/* Continue with parent */
		tlinfo = tlinfo->parent_link;
	}

	return closest_backup;
}

/*
 * Find oldest backup in given timeline
 * to determine what WAL segments of this timeline
 * are reachable from backups belonging to it.
 *
 * If such backup doesn't exist, it means that
 * there is no backups on this timeline. Return NULL.
 */
pgBackup*
get_oldest_backup(timelineInfo *tlinfo)
{
	pgBackup *oldest_backup = NULL;
	int i;
	parray *backup_list = tlinfo->backups;

	if (backup_list != NULL)
	{
		for (i = 0; i < parray_num(backup_list); i++)
		{
			pgBackup   *backup = parray_get(backup_list, i);

			/* Backups with invalid START LSN can be safely skipped */
			if (XLogRecPtrIsInvalid(backup->start_lsn) ||
				!XRecOffIsValid(backup->start_lsn))
				continue;

			/*
			 * Check if backup is older than current candidate.
			 * Here we use start_lsn for comparison, because backup that
			 * started earlier needs more WAL.
			 */
			if (!oldest_backup || backup->start_lsn < oldest_backup->start_lsn)
				oldest_backup = backup;
		}
	}

	return oldest_backup;
}

/*
 * Overwrite backup metadata.
 */
void
do_set_backup(const char *instance_name, time_t backup_id,
			  pgSetBackupParams *set_backup_params)
{
	pgBackup	*target_backup = NULL;
	parray 		*backup_list = NULL;

	if (!set_backup_params)
		elog(ERROR, "Nothing to set by 'set-backup' command");

	backup_list = catalog_get_backup_list(instance_name, backup_id);
	if (parray_num(backup_list) != 1)
		elog(ERROR, "Failed to find backup %s", base36enc(backup_id));

	target_backup = (pgBackup *) parray_get(backup_list, 0);

	/* Pin or unpin backup if requested */
	if (set_backup_params->ttl >= 0 || set_backup_params->expire_time > 0)
		pin_backup(target_backup, set_backup_params);

	if (set_backup_params->note)
		add_note(target_backup, set_backup_params->note);
}

/*
 * Set 'expire-time' attribute based on set_backup_params, or unpin backup
 * if ttl is equal to zero.
 */
void
pin_backup(pgBackup	*target_backup, pgSetBackupParams *set_backup_params)
{

	/* sanity, backup must have positive recovery-time */
	if (target_backup->recovery_time <= 0)
		elog(ERROR, "Failed to set 'expire-time' for backup %s: invalid 'recovery-time'",
						base36enc(target_backup->backup_id));

	/* Pin comes from ttl */
	if (set_backup_params->ttl > 0)
		target_backup->expire_time = target_backup->recovery_time + set_backup_params->ttl;
	/* Unpin backup */
	else if (set_backup_params->ttl == 0)
	{
		/* If backup was not pinned in the first place,
		 * then there is nothing to unpin.
		 */
		if (target_backup->expire_time == 0)
		{
			elog(WARNING, "Backup %s is not pinned, nothing to unpin",
									base36enc(target_backup->start_time));
			return;
		}
		target_backup->expire_time = 0;
	}
	/* Pin comes from expire-time */
	else if (set_backup_params->expire_time > 0)
		target_backup->expire_time = set_backup_params->expire_time;
	else
		/* nothing to do */
		return;

	/* Update backup.control */
	write_backup(target_backup, true);

	if (set_backup_params->ttl > 0 || set_backup_params->expire_time > 0)
	{
		char	expire_timestamp[100];

		time2iso(expire_timestamp, lengthof(expire_timestamp), target_backup->expire_time, false);
		elog(INFO, "Backup %s is pinned until '%s'", base36enc(target_backup->start_time),
														expire_timestamp);
	}
	else
		elog(INFO, "Backup %s is unpinned", base36enc(target_backup->start_time));

	return;
}

/*
 * Add note to backup metadata or unset already existing note.
 * It is a job of the caller to make sure that note is not NULL.
 */
void
add_note(pgBackup *target_backup, char *note)
{

	char *note_string;

	/* unset note */
	if (pg_strcasecmp(note, "none") == 0)
	{
		target_backup->note = NULL;
		elog(INFO, "Removing note from backup %s",
				base36enc(target_backup->start_time));
	}
	else
	{
		/* Currently we do not allow string with newlines as note,
		 * because it will break parsing of backup.control.
		 * So if user provides string like this "aaa\nbbbbb",
		 * we save only "aaa"
		 * Example: tests.set_backup.SetBackupTest.test_add_note_newlines
		 */
		note_string = pgut_malloc(MAX_NOTE_SIZE);
		sscanf(note, "%[^\n]", note_string);

		target_backup->note = note_string;
		elog(INFO, "Adding note to backup %s: '%s'",
				base36enc(target_backup->start_time), target_backup->note);
	}

	/* Update backup.control */
	write_backup(target_backup, true);
}

/*
 * Write information about backup.in to stream "out".
 */
void
pgBackupWriteControl(FILE *out, pgBackup *backup, bool utc)
{
	char		timestamp[100];

	fio_fprintf(out, "#Configuration\n");
	fio_fprintf(out, "backup-mode = %s\n", pgBackupGetBackupMode(backup));
	fio_fprintf(out, "stream = %s\n", backup->stream ? "true" : "false");
	fio_fprintf(out, "compress-alg = %s\n",
			deparse_compress_alg(backup->compress_alg));
	fio_fprintf(out, "compress-level = %d\n", backup->compress_level);
	fio_fprintf(out, "from-replica = %s\n", backup->from_replica ? "true" : "false");

	fio_fprintf(out, "\n#Compatibility\n");
	fio_fprintf(out, "block-size = %u\n", backup->block_size);
	fio_fprintf(out, "xlog-block-size = %u\n", backup->wal_block_size);
	fio_fprintf(out, "checksum-version = %u\n", backup->checksum_version);
	if (backup->program_version[0] != '\0')
		fio_fprintf(out, "program-version = %s\n", backup->program_version);
	if (backup->server_version[0] != '\0')
		fio_fprintf(out, "server-version = %s\n", backup->server_version);

	fio_fprintf(out, "\n#Result backup info\n");
	fio_fprintf(out, "timelineid = %d\n", backup->tli);
	/* LSN returned by pg_start_backup */
	fio_fprintf(out, "start-lsn = %X/%X\n",
			(uint32) (backup->start_lsn >> 32),
			(uint32) backup->start_lsn);
	/* LSN returned by pg_stop_backup */
	fio_fprintf(out, "stop-lsn = %X/%X\n",
			(uint32) (backup->stop_lsn >> 32),
			(uint32) backup->stop_lsn);

	time2iso(timestamp, lengthof(timestamp), backup->start_time, utc);
	fio_fprintf(out, "start-time = '%s'\n", timestamp);
	if (backup->merge_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->merge_time, utc);
		fio_fprintf(out, "merge-time = '%s'\n", timestamp);
	}
	if (backup->end_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->end_time, utc);
		fio_fprintf(out, "end-time = '%s'\n", timestamp);
	}
	fio_fprintf(out, "recovery-xid = " XID_FMT "\n", backup->recovery_xid);
	if (backup->recovery_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time, utc);
		fio_fprintf(out, "recovery-time = '%s'\n", timestamp);
	}
	if (backup->expire_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->expire_time, utc);
		fio_fprintf(out, "expire-time = '%s'\n", timestamp);
	}

	if (backup->merge_dest_backup != 0)
		fio_fprintf(out, "merge-dest-id = '%s'\n", base36enc(backup->merge_dest_backup));

	/*
	 * Size of PGDATA directory. The size does not include size of related
	 * WAL segments in archive 'wal' directory.
	 */
	if (backup->data_bytes != BYTES_INVALID)
		fio_fprintf(out, "data-bytes = " INT64_FORMAT "\n", backup->data_bytes);

	if (backup->wal_bytes != BYTES_INVALID)
		fio_fprintf(out, "wal-bytes = " INT64_FORMAT "\n", backup->wal_bytes);

	if (backup->uncompressed_bytes >= 0)
		fio_fprintf(out, "uncompressed-bytes = " INT64_FORMAT "\n", backup->uncompressed_bytes);

	if (backup->pgdata_bytes >= 0)
		fio_fprintf(out, "pgdata-bytes = " INT64_FORMAT "\n", backup->pgdata_bytes);

	fio_fprintf(out, "status = %s\n", status2str(backup->status));

	/* 'parent_backup' is set if it is incremental backup */
	if (backup->parent_backup != 0)
		fio_fprintf(out, "parent-backup-id = '%s'\n", base36enc(backup->parent_backup));

	/* print connection info except password */
	if (backup->primary_conninfo)
		fio_fprintf(out, "primary_conninfo = '%s'\n", backup->primary_conninfo);

	/* print external directories list */
	if (backup->external_dir_str)
		fio_fprintf(out, "external-dirs = '%s'\n", backup->external_dir_str);

	if (backup->note)
		fio_fprintf(out, "note = '%s'\n", backup->note);

	if (backup->content_crc != 0)
		fio_fprintf(out, "content-crc = %u\n", backup->content_crc);

}

/*
 * Save the backup content into BACKUP_CONTROL_FILE.
 * Flag strict allows to ignore "out of space" error
 * when attempting to lock backup. Only delete is allowed
 * to use this functionality.
 */
void
write_backup(pgBackup *backup, bool strict)
{
	FILE   *fp = NULL;
	char    path[MAXPGPATH];
	char    path_temp[MAXPGPATH];
	char    buf[8192];

	join_path_components(path, backup->root_dir, BACKUP_CONTROL_FILE);
	snprintf(path_temp, sizeof(path_temp), "%s.tmp", path);

	fp = fopen(path_temp, PG_BINARY_W);
	if (fp == NULL)
		elog(ERROR, "Cannot open control file \"%s\": %s",
			 path_temp, strerror(errno));

	if (chmod(path_temp, FILE_PERMISSION) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", path_temp,
			 strerror(errno));

	setvbuf(fp, buf, _IOFBF, sizeof(buf));

	pgBackupWriteControl(fp, backup, true);

	/* Ignore 'out of space' error in lax mode */
	if (fflush(fp) != 0)
	{
		int elevel = ERROR;
		int save_errno = errno;

		if (!strict && (errno == ENOSPC))
			elevel = WARNING;

		elog(elevel, "Cannot flush control file \"%s\": %s",
				 path_temp, strerror(save_errno));

		if (!strict && (save_errno == ENOSPC))
		{
			fclose(fp);
			fio_unlink(path_temp, FIO_BACKUP_HOST);
			return;
		}
	}

	if (fclose(fp) != 0)
		elog(ERROR, "Cannot close control file \"%s\": %s",
			 path_temp, strerror(errno));

	if (fio_sync(path_temp, FIO_BACKUP_HOST) < 0)
		elog(ERROR, "Cannot sync control file \"%s\": %s",
			 path_temp, strerror(errno));

	if (rename(path_temp, path) < 0)
		elog(ERROR, "Cannot rename file \"%s\" to \"%s\": %s",
			 path_temp, path, strerror(errno));
}

/*
 * Output the list of files to backup catalog DATABASE_FILE_LIST
 */
void
write_backup_filelist(pgBackup *backup, parray *files, const char *root,
					  parray *external_list, bool sync)
{
	FILE	   *out;
	char		control_path[MAXPGPATH];
	char		control_path_temp[MAXPGPATH];
	size_t		i = 0;
	#define BUFFERSZ 1024*1024
	char		*buf;
	int64 		backup_size_on_disk = 0;
	int64 		uncompressed_size_on_disk = 0;
	int64 		wal_size_on_disk = 0;

	join_path_components(control_path, backup->root_dir, DATABASE_FILE_LIST);
	snprintf(control_path_temp, sizeof(control_path_temp), "%s.tmp", control_path);

	out = fopen(control_path_temp, PG_BINARY_W);
	if (out == NULL)
		elog(ERROR, "Cannot open file list \"%s\": %s", control_path_temp,
			 strerror(errno));

	if (chmod(control_path_temp, FILE_PERMISSION) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", control_path_temp,
			 strerror(errno));

	buf = pgut_malloc(BUFFERSZ);
	setvbuf(out, buf, _IOFBF, BUFFERSZ);

	if (sync)
		INIT_FILE_CRC32(true, backup->content_crc);

	/* print each file in the list */
	for (i = 0; i < parray_num(files); i++)
	{
		int       len = 0;
		char      line[BLCKSZ];
		pgFile   *file = (pgFile *) parray_get(files, i);

		/* Ignore disappeared file */
		if (file->write_size == FILE_NOT_FOUND)
			continue;

		if (S_ISDIR(file->mode))
		{
			backup_size_on_disk += 4096;
			uncompressed_size_on_disk += 4096;
		}

		/* Count the amount of the data actually copied */
		if (S_ISREG(file->mode) && file->write_size > 0)
		{
			/*
			 * Size of WAL files in 'pg_wal' is counted separately
			 * TODO: in 3.0 add attribute is_walfile
			 */
			if (IsXLogFileName(file->name) && file->external_dir_num == 0)
				wal_size_on_disk += file->write_size;
			else
			{
				backup_size_on_disk += file->write_size;
				uncompressed_size_on_disk += file->uncompressed_size;
			}
		}

		len = sprintf(line, "{\"path\":\"%s\", \"size\":\"" INT64_FORMAT "\", "
					 "\"mode\":\"%u\", \"is_datafile\":\"%u\", "
					 "\"is_cfs\":\"%u\", \"crc\":\"%u\", "
					 "\"compress_alg\":\"%s\", \"external_dir_num\":\"%d\", "
					 "\"dbOid\":\"%u\"",
					file->rel_path, file->write_size, file->mode,
					file->is_datafile ? 1 : 0,
					file->is_cfs ? 1 : 0,
					file->crc,
					deparse_compress_alg(file->compress_alg),
					file->external_dir_num,
					file->dbOid);

		if (file->is_datafile)
			len += sprintf(line+len, ",\"segno\":\"%d\"", file->segno);

		if (file->linked)
			len += sprintf(line+len, ",\"linked\":\"%s\"", file->linked);

		if (file->n_blocks > 0)
			len += sprintf(line+len, ",\"n_blocks\":\"%i\"", file->n_blocks);

		if (file->n_headers > 0)
		{
			len += sprintf(line+len, ",\"n_headers\":\"%i\"", file->n_headers);
			len += sprintf(line+len, ",\"hdr_crc\":\"%u\"", file->hdr_crc);
			len += sprintf(line+len, ",\"hdr_off\":\"%llu\"", file->hdr_off);
			len += sprintf(line+len, ",\"hdr_size\":\"%i\"", file->hdr_size);
		}

		sprintf(line+len, "}\n");

		if (sync)
			COMP_FILE_CRC32(true, backup->content_crc, line, strlen(line));

		fprintf(out, "%s", line);
	}

	if (sync)
		FIN_FILE_CRC32(true, backup->content_crc);

	if (fflush(out) != 0)
		elog(ERROR, "Cannot flush file list \"%s\": %s",
			 control_path_temp, strerror(errno));

	if (sync && fsync(fileno(out)) < 0)
		elog(ERROR, "Cannot sync file list \"%s\": %s",
			 control_path_temp, strerror(errno));

	if (fclose(out) != 0)
		elog(ERROR, "Cannot close file list \"%s\": %s",
			 control_path_temp, strerror(errno));

	if (rename(control_path_temp, control_path) < 0)
		elog(ERROR, "Cannot rename file \"%s\" to \"%s\": %s",
			 control_path_temp, control_path, strerror(errno));

	/* use extra variable to avoid reset of previous data_bytes value in case of error */
	backup->data_bytes = backup_size_on_disk;
	backup->uncompressed_bytes = uncompressed_size_on_disk;

	if (backup->stream)
		backup->wal_bytes = wal_size_on_disk;

	free(buf);
}

/*
 * Read BACKUP_CONTROL_FILE and create pgBackup.
 *  - Comment starts with ';'.
 *  - Do not care section.
 */
static pgBackup *
readBackupControlFile(const char *path)
{
	pgBackup   *backup = pgut_new(pgBackup);
	char	   *backup_mode = NULL;
	char	   *start_lsn = NULL;
	char	   *stop_lsn = NULL;
	char	   *status = NULL;
	char	   *parent_backup = NULL;
	char	   *merge_dest_backup = NULL;
	char	   *program_version = NULL;
	char	   *server_version = NULL;
	char	   *compress_alg = NULL;
	int			parsed_options;

	ConfigOption options[] =
	{
		{'s', 0, "backup-mode",			&backup_mode, SOURCE_FILE_STRICT},
		{'u', 0, "timelineid",			&backup->tli, SOURCE_FILE_STRICT},
		{'s', 0, "start-lsn",			&start_lsn, SOURCE_FILE_STRICT},
		{'s', 0, "stop-lsn",			&stop_lsn, SOURCE_FILE_STRICT},
		{'t', 0, "start-time",			&backup->start_time, SOURCE_FILE_STRICT},
		{'t', 0, "merge-time",			&backup->merge_time, SOURCE_FILE_STRICT},
		{'t', 0, "end-time",			&backup->end_time, SOURCE_FILE_STRICT},
		{'U', 0, "recovery-xid",		&backup->recovery_xid, SOURCE_FILE_STRICT},
		{'t', 0, "recovery-time",		&backup->recovery_time, SOURCE_FILE_STRICT},
		{'t', 0, "expire-time",			&backup->expire_time, SOURCE_FILE_STRICT},
		{'I', 0, "data-bytes",			&backup->data_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "wal-bytes",			&backup->wal_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "uncompressed-bytes",	&backup->uncompressed_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "pgdata-bytes",		&backup->pgdata_bytes, SOURCE_FILE_STRICT},
		{'u', 0, "block-size",			&backup->block_size, SOURCE_FILE_STRICT},
		{'u', 0, "xlog-block-size",		&backup->wal_block_size, SOURCE_FILE_STRICT},
		{'u', 0, "checksum-version",	&backup->checksum_version, SOURCE_FILE_STRICT},
		{'s', 0, "program-version",		&program_version, SOURCE_FILE_STRICT},
		{'s', 0, "server-version",		&server_version, SOURCE_FILE_STRICT},
		{'b', 0, "stream",				&backup->stream, SOURCE_FILE_STRICT},
		{'s', 0, "status",				&status, SOURCE_FILE_STRICT},
		{'s', 0, "parent-backup-id",	&parent_backup, SOURCE_FILE_STRICT},
		{'s', 0, "merge-dest-id",		&merge_dest_backup, SOURCE_FILE_STRICT},
		{'s', 0, "compress-alg",		&compress_alg, SOURCE_FILE_STRICT},
		{'u', 0, "compress-level",		&backup->compress_level, SOURCE_FILE_STRICT},
		{'b', 0, "from-replica",		&backup->from_replica, SOURCE_FILE_STRICT},
		{'s', 0, "primary-conninfo",	&backup->primary_conninfo, SOURCE_FILE_STRICT},
		{'s', 0, "external-dirs",		&backup->external_dir_str, SOURCE_FILE_STRICT},
		{'s', 0, "note",				&backup->note, SOURCE_FILE_STRICT},
		{'u', 0, "content-crc",			&backup->content_crc, SOURCE_FILE_STRICT},
		{0}
	};

	pgBackupInit(backup);
	if (fio_access(path, F_OK, FIO_BACKUP_HOST) != 0)
	{
		elog(WARNING, "Control file \"%s\" doesn't exist", path);
		pgBackupFree(backup);
		return NULL;
	}

	parsed_options = config_read_opt(path, options, WARNING, true, true);

	if (parsed_options == 0)
	{
		elog(WARNING, "Control file \"%s\" is empty", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup->start_time == 0)
	{
		elog(WARNING, "Invalid ID/start-time, control file \"%s\" is corrupted", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup_mode)
	{
		backup->backup_mode = parse_backup_mode(backup_mode);
		free(backup_mode);
	}

	if (start_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(start_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->start_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid START_LSN \"%s\"", start_lsn);
		free(start_lsn);
	}

	if (stop_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(stop_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->stop_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid STOP_LSN \"%s\"", stop_lsn);
		free(stop_lsn);
	}

	if (status)
	{
		if (strcmp(status, "OK") == 0)
			backup->status = BACKUP_STATUS_OK;
		else if (strcmp(status, "ERROR") == 0)
			backup->status = BACKUP_STATUS_ERROR;
		else if (strcmp(status, "RUNNING") == 0)
			backup->status = BACKUP_STATUS_RUNNING;
		else if (strcmp(status, "MERGING") == 0)
			backup->status = BACKUP_STATUS_MERGING;
		else if (strcmp(status, "MERGED") == 0)
			backup->status = BACKUP_STATUS_MERGED;
		else if (strcmp(status, "DELETING") == 0)
			backup->status = BACKUP_STATUS_DELETING;
		else if (strcmp(status, "DELETED") == 0)
			backup->status = BACKUP_STATUS_DELETED;
                else if (strcmp(status, "DETACHING") == 0)
                        backup->status = BACKUP_STATUS_DETACHING;
                else if (strcmp(status, "DETACHED") == 0)
                        backup->status = BACKUP_STATUS_DETACHED;
		else if (strcmp(status, "DONE") == 0)
			backup->status = BACKUP_STATUS_DONE;
		else if (strcmp(status, "ORPHAN") == 0)
			backup->status = BACKUP_STATUS_ORPHAN;
		else if (strcmp(status, "CORRUPT") == 0)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			elog(WARNING, "Invalid STATUS \"%s\"", status);
		free(status);
	}

	if (parent_backup)
	{
		backup->parent_backup = base36dec(parent_backup);
		free(parent_backup);
	}

	if (merge_dest_backup)
	{
		backup->merge_dest_backup = base36dec(merge_dest_backup);
		free(merge_dest_backup);
	}

	if (program_version)
	{
		strlcpy(backup->program_version, program_version,
				sizeof(backup->program_version));
		pfree(program_version);
	}

	if (server_version)
	{
		strlcpy(backup->server_version, server_version,
				sizeof(backup->server_version));
		pfree(server_version);
	}

	if (compress_alg)
		backup->compress_alg = parse_compress_alg(compress_alg);

	return backup;
}

BackupMode
parse_backup_mode(const char *value)
{
	const char *v = value;
	size_t		len;

	/* Skip all spaces detected */
	while (IsSpace(*v))
		v++;
	len = strlen(v);

	if (len > 0 && pg_strncasecmp("full", v, len) == 0)
		return BACKUP_MODE_FULL;
	else if (len > 0 && pg_strncasecmp("page", v, len) == 0)
		return BACKUP_MODE_DIFF_PAGE;
	else if (len > 0 && pg_strncasecmp("ptrack", v, len) == 0)
		return BACKUP_MODE_DIFF_PTRACK;
	else if (len > 0 && pg_strncasecmp("delta", v, len) == 0)
		return BACKUP_MODE_DIFF_DELTA;

	/* Backup mode is invalid, so leave with an error */
	elog(ERROR, "invalid backup-mode \"%s\"", value);
	return BACKUP_MODE_INVALID;
}

const char *
deparse_backup_mode(BackupMode mode)
{
	switch (mode)
	{
		case BACKUP_MODE_FULL:
			return "full";
		case BACKUP_MODE_DIFF_PAGE:
			return "page";
		case BACKUP_MODE_DIFF_PTRACK:
			return "ptrack";
		case BACKUP_MODE_DIFF_DELTA:
			return "delta";
		case BACKUP_MODE_INVALID:
			return "invalid";
	}

	return NULL;
}

CompressAlg
parse_compress_alg(const char *arg)
{
	size_t		len;

	/* Skip all spaces detected */
	while (isspace((unsigned char)*arg))
		arg++;
	len = strlen(arg);

	if (len == 0)
		elog(ERROR, "compress algorithm is empty");

	if (pg_strncasecmp("zlib", arg, len) == 0)
		return ZLIB_COMPRESS;
	else if (pg_strncasecmp("pglz", arg, len) == 0)
		return PGLZ_COMPRESS;
	else if (pg_strncasecmp("none", arg, len) == 0)
		return NONE_COMPRESS;
	else
		elog(ERROR, "invalid compress algorithm value \"%s\"", arg);

	return NOT_DEFINED_COMPRESS;
}

const char*
deparse_compress_alg(int alg)
{
	switch (alg)
	{
		case NONE_COMPRESS:
		case NOT_DEFINED_COMPRESS:
			return "none";
		case ZLIB_COMPRESS:
			return "zlib";
		case PGLZ_COMPRESS:
			return "pglz";
	}

	return NULL;
}

/*
 * Fill PGNodeInfo struct with default values.
 */
void
pgNodeInit(PGNodeInfo *node)
{
	node->block_size = 0;
	node->wal_block_size = 0;
	node->checksum_version = 0;

	node->is_superuser = false;
	node->pgpro_support = false;

	node->server_version = 0;
	node->server_version_str[0] = '\0';

	node->ptrack_version_num = 0;
	node->is_ptrack_enable = false;
	node->ptrack_schema = NULL;
}

/*
 * Fill pgBackup struct with default values.
 */
void
pgBackupInit(pgBackup *backup)
{
	backup->backup_id = INVALID_BACKUP_ID;
	backup->backup_mode = BACKUP_MODE_INVALID;
	backup->status = BACKUP_STATUS_INVALID;
	backup->tli = 0;
	backup->start_lsn = 0;
	backup->stop_lsn = 0;
	backup->start_time = (time_t) 0;
	backup->merge_time = (time_t) 0;
	backup->end_time = (time_t) 0;
	backup->recovery_xid = 0;
	backup->recovery_time = (time_t) 0;
	backup->expire_time = (time_t) 0;

	backup->data_bytes = BYTES_INVALID;
	backup->wal_bytes = BYTES_INVALID;
	backup->uncompressed_bytes = 0;
	backup->pgdata_bytes = 0;

	backup->compress_alg = COMPRESS_ALG_DEFAULT;
	backup->compress_level = COMPRESS_LEVEL_DEFAULT;

	backup->block_size = BLCKSZ;
	backup->wal_block_size = XLOG_BLCKSZ;
	backup->checksum_version = 0;

	backup->stream = false;
	backup->from_replica = false;
	backup->parent_backup = INVALID_BACKUP_ID;
	backup->merge_dest_backup = INVALID_BACKUP_ID;
	backup->parent_backup_link = NULL;
	backup->primary_conninfo = NULL;
	backup->program_version[0] = '\0';
	backup->server_version[0] = '\0';
	backup->external_dir_str = NULL;
	backup->root_dir = NULL;
	backup->database_dir = NULL;
	backup->files = NULL;
	backup->note = NULL;
	backup->content_crc = 0;
}

/* free pgBackup object */
void
pgBackupFree(void *backup)
{
	pgBackup *b = (pgBackup *) backup;

	pg_free(b->primary_conninfo);
	pg_free(b->external_dir_str);
	pg_free(b->root_dir);
	pg_free(b->database_dir);
	pg_free(b->note);
	pg_free(backup);
}

/* Compare two pgBackup with their IDs (start time) in ascending order */
int
pgBackupCompareId(const void *l, const void *r)
{
	pgBackup *lp = *(pgBackup **)l;
	pgBackup *rp = *(pgBackup **)r;

	if (lp->start_time > rp->start_time)
		return 1;
	else if (lp->start_time < rp->start_time)
		return -1;
	else
		return 0;
}

/* Compare two pgBackup with their IDs in descending order */
int
pgBackupCompareIdDesc(const void *l, const void *r)
{
	return -pgBackupCompareId(l, r);
}

/*
 * Construct absolute path of the backup directory.
 * If subdir is not NULL, it will be appended after the path.
 */
void
pgBackupGetPath(const pgBackup *backup, char *path, size_t len, const char *subdir)
{
	pgBackupGetPath2(backup, path, len, subdir, NULL);
}

/*
 * Construct absolute path of the backup directory.
 * Append "subdir1" and "subdir2" to the backup directory.
 */
void
pgBackupGetPath2(const pgBackup *backup, char *path, size_t len,
				 const char *subdir1, const char *subdir2)
{
	/* If "subdir1" is NULL do not check "subdir2" */
	if (!subdir1)
		snprintf(path, len, "%s/%s", backup_instance_path,
				 base36enc(backup->start_time));
	else if (!subdir2)
		snprintf(path, len, "%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1);
	/* "subdir1" and "subdir2" is not NULL */
	else
		snprintf(path, len, "%s/%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1, subdir2);
}

/*
 * independent from global variable backup_instance_path
 * Still depends from backup_path
 */
void
pgBackupGetPathInInstance(const char *instance_name,
				 const pgBackup *backup, char *path, size_t len,
				 const char *subdir1, const char *subdir2)
{
	char		backup_instance_path[MAXPGPATH];

	sprintf(backup_instance_path, "%s/%s/%s",
				backup_path, BACKUPS_DIR, instance_name);

	/* If "subdir1" is NULL do not check "subdir2" */
	if (!subdir1)
		snprintf(path, len, "%s/%s", backup_instance_path,
				 base36enc(backup->start_time));
	else if (!subdir2)
		snprintf(path, len, "%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1);
	/* "subdir1" and "subdir2" is not NULL */
	else
		snprintf(path, len, "%s/%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1, subdir2);
}

/*
 * Check if multiple backups consider target backup to be their direct parent
 */
bool
is_prolific(parray *backup_list, pgBackup *target_backup)
{
	int i;
	int child_counter = 0;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		/* consider only OK and DONE backups */
		if (tmp_backup->parent_backup == target_backup->start_time &&
			(tmp_backup->status == BACKUP_STATUS_OK ||
			 tmp_backup->status == BACKUP_STATUS_DONE))
		{
			child_counter++;
			if (child_counter > 1)
				return true;
		}
	}

	return false;
}

/*
 * Find parent base FULL backup for current backup using parent_backup_link
 */
pgBackup*
find_parent_full_backup(pgBackup *current_backup)
{
	pgBackup   *base_full_backup = NULL;
	base_full_backup = current_backup;

	/* sanity */
	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	while (base_full_backup->parent_backup_link != NULL)
	{
		base_full_backup = base_full_backup->parent_backup_link;
	}

	if (base_full_backup->backup_mode != BACKUP_MODE_FULL)
	{
		if (base_full_backup->parent_backup)
			elog(WARNING, "Backup %s is missing",
				 base36enc(base_full_backup->parent_backup));
		else
			elog(WARNING, "Failed to find parent FULL backup for %s",
				 base36enc(current_backup->start_time));
		return NULL;
	}

	return base_full_backup;
}

/*
 * Iterate over parent chain and look for any problems.
 * Return 0 if chain is broken.
 *  result_backup must contain oldest existing backup after missing backup.
 *  we have no way to know if there are multiple missing backups.
 * Return 1 if chain is intact, but at least one backup is !OK.
 *  result_backup must contain oldest !OK backup.
 * Return 2 if chain is intact and all backups are OK.
 *	result_backup must contain FULL backup on which chain is based.
 */
int
scan_parent_chain(pgBackup *current_backup, pgBackup **result_backup)
{
	pgBackup   *target_backup = NULL;
	pgBackup   *invalid_backup = NULL;

	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	target_backup = current_backup;

	while (target_backup->parent_backup_link)
	{
		if (target_backup->status != BACKUP_STATUS_OK &&
			  target_backup->status != BACKUP_STATUS_DONE)
			/* oldest invalid backup in parent chain */
			invalid_backup = target_backup;


		target_backup = target_backup->parent_backup_link;
	}

	/* Previous loop will skip FULL backup because his parent_backup_link is NULL */
	if (target_backup->backup_mode == BACKUP_MODE_FULL &&
		(target_backup->status != BACKUP_STATUS_OK &&
		target_backup->status != BACKUP_STATUS_DONE))
	{
		invalid_backup = target_backup;
	}

	/* found chain end and oldest backup is not FULL */
	if (target_backup->backup_mode != BACKUP_MODE_FULL)
	{
		/* Set oldest child backup in chain */
		*result_backup = target_backup;
		return ChainIsBroken;
	}

	/* chain is ok, but some backups are invalid */
	if (invalid_backup)
	{
		*result_backup = invalid_backup;
		return ChainIsInvalid;
	}

	*result_backup = target_backup;
	return ChainIsOk;
}

/*
 * Determine if child_backup descend from parent_backup
 * This check DO NOT(!!!) guarantee that parent chain is intact,
 * because parent_backup can be missing.
 * If inclusive is true, then child_backup counts as a child of himself
 * if parent_backup_time is start_time of child_backup.
 */
bool
is_parent(time_t parent_backup_time, pgBackup *child_backup, bool inclusive)
{
	if (!child_backup)
		elog(ERROR, "Target backup cannot be NULL");

	if (inclusive && child_backup->start_time == parent_backup_time)
		return true;

	while (child_backup->parent_backup_link &&
			child_backup->parent_backup != parent_backup_time)
	{
		child_backup = child_backup->parent_backup_link;
	}

	if (child_backup->parent_backup == parent_backup_time)
		return true;

	//if (inclusive && child_backup->start_time == parent_backup_time)
	//	return true;

	return false;
}

/*
 * Return backup index number.
 * Note: this index number holds true until new sorting of backup list
 */
int
get_backup_index_number(parray *backup_list, pgBackup *backup)
{
	int i;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		if (tmp_backup->start_time == backup->start_time)
			return i;
	}
	elog(WARNING, "Failed to find backup %s", base36enc(backup->start_time));
	return -1;
}

/* On backup_list lookup children of target_backup and append them to append_list */
void
append_children(parray *backup_list, pgBackup *target_backup, parray *append_list)
{
	int i;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *) parray_get(backup_list, i);

		/* check if backup is descendant of target backup */
		if (is_parent(target_backup->start_time, backup, false))
		{
			/* if backup is already in the list, then skip it */
			if (!parray_contains(append_list, backup))
				parray_append(append_list, backup);
		}
	}
}
