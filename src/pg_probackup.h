/*-------------------------------------------------------------------------
 *
 * pg_probackup.h: Backup/Recovery manager for PostgreSQL.
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2018, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROBACKUP_H
#define PG_PROBACKUP_H


#include "postgres_fe.h"
#include "libpq-fe.h"
#include "libpq-int.h"

#include "access/xlog_internal.h"
#include "utils/pg_crc.h"

#if PG_VERSION_NUM >= 120000
#include "common/logging.h"
#endif

#ifdef FRONTEND
#undef FRONTEND
#include <port/atomics.h>
#define FRONTEND
#else
#include <port/atomics.h>
#endif

#include "utils/configuration.h"
#include "utils/logger.h"
#include "utils/remote.h"
#include "utils/parray.h"
#include "utils/pgut.h"
#include "utils/file.h"

#include "datapagemap.h"
#include "utils/thread.h"

#include "pg_probackup_state.h"


#ifdef WIN32
#define __thread __declspec(thread)
#else
#include <pthread.h>
#endif

/* Wrap the code that we're going to delete after refactoring in this define*/
#define REFACTORE_ME

/* pgut client variables and full path */
extern const char  *PROGRAM_NAME;
extern const char  *PROGRAM_NAME_FULL;
extern const char  *PROGRAM_FULL_PATH;
extern const char  *PROGRAM_URL;
extern const char  *PROGRAM_EMAIL;

/* Directory/File names */
#define DATABASE_DIR			"database"
#define BACKUPS_DIR				"backups"
#define WAL_SUBDIR				"wal"
#if PG_VERSION_NUM >= 100000
#define PG_XLOG_DIR				"pg_wal"
#define PG_LOG_DIR 				"log"
#else
#define PG_XLOG_DIR				"pg_xlog"
#define PG_LOG_DIR 				"pg_log"
#endif
#define PG_TBLSPC_DIR			"pg_tblspc"
#define PG_GLOBAL_DIR			"global"
#define BACKUP_CONTROL_FILE		"backup.control"
#define BACKUP_CATALOG_CONF_FILE	"pg_probackup.conf"
#define BACKUP_LOCK_FILE		"backup.pid"
#define BACKUP_RO_LOCK_FILE		"backup_ro.pid"
#define DATABASE_FILE_LIST		"backup_content.control"
#define PG_BACKUP_LABEL_FILE	"backup_label"
#define PG_TABLESPACE_MAP_FILE	"tablespace_map"
#define RELMAPPER_FILENAME		"pg_filenode.map"
#define EXTERNAL_DIR			"external_directories/externaldir"
#define DATABASE_MAP			"database_map"
#define HEADER_MAP  			"page_header_map"
#define HEADER_MAP_TMP  		"page_header_map_tmp"
#define XLOG_CONTROL_BAK_FILE		XLOG_CONTROL_FILE".pbk.bak"

/* Timeout defaults */
#define ARCHIVE_TIMEOUT_DEFAULT		300
#define REPLICA_TIMEOUT_DEFAULT		300
#define LOCK_TIMEOUT				60
#define LOCK_STALE_TIMEOUT			30
#define LOG_FREQ					10

/* Directory/File permission */
#define DIR_PERMISSION		(0700)
#define FILE_PERMISSION		(0600)

/* 64-bit xid support for PGPRO_EE */
#ifndef PGPRO_EE
#define XID_FMT "%u"
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif

/* stdio buffer size */
#define STDIO_BUFSIZE 65536

#define ERRMSG_MAX_LEN 2048
#define CHUNK_SIZE (128 * 1024)
#define LARGE_CHUNK_SIZE (4 * 1024 * 1024)
#define OUT_BUF_SIZE (512 * 1024)

/* retry attempts */
#define PAGE_READ_ATTEMPTS 300

/* max size of note, that can be added to backup */
#define MAX_NOTE_SIZE 1024

/* Check if an XLogRecPtr value is pointed to 0 offset */
#define XRecOffIsNull(xlrp) \
		((xlrp) % XLOG_BLCKSZ == 0)

/* Text Coloring macro */
#define TC_LEN 11
#define TC_RED "\033[0;31m"
#define TC_RED_BOLD "\033[1;31m"
#define TC_BLUE "\033[0;34m"
#define TC_BLUE_BOLD "\033[1;34m"
#define TC_GREEN "\033[0;32m"
#define TC_GREEN_BOLD "\033[1;32m"
#define TC_YELLOW "\033[0;33m"
#define TC_YELLOW_BOLD "\033[1;33m"
#define TC_MAGENTA "\033[0;35m"
#define TC_MAGENTA_BOLD "\033[1;35m"
#define TC_CYAN "\033[0;36m"
#define TC_CYAN_BOLD "\033[1;36m"
#define TC_RESET "\033[0m"


typedef struct RedoParams
{
	TimeLineID  tli;
	XLogRecPtr  lsn;
	uint32      checksum_version;
} RedoParams;

typedef struct PageState
{
	uint16  checksum;
	XLogRecPtr  lsn;
} PageState;

typedef struct db_map_entry
{
	Oid dbOid;
	char *datname;
} db_map_entry;

/* State of pgdata in the context of its compatibility for incremental restore  */
typedef enum DestDirIncrCompatibility
{
	POSTMASTER_IS_RUNNING,
	SYSTEM_ID_MISMATCH,
	BACKUP_LABEL_EXISTS,
	DEST_IS_NOT_OK,
	DEST_OK
} DestDirIncrCompatibility;

typedef enum IncrRestoreMode
{
	INCR_NONE,
	INCR_CHECKSUM,
	INCR_LSN
} IncrRestoreMode;

typedef enum PartialRestoreType
{
	NONE,
	INCLUDE,
	EXCLUDE,
} PartialRestoreType;

typedef enum RecoverySettingsMode
{
	DEFAULT,	/* not set */
	DONTWRITE,	/* explicitly forbid to update recovery settings */
				//TODO Should we always clean/preserve old recovery settings,
				// or make it configurable?
	PITR_REQUESTED, /* can be set based on other parameters
	                 * if not explicitly forbidden */
} RecoverySettingsMode;

typedef enum CompressAlg
{
	NOT_DEFINED_COMPRESS = 0,
	NONE_COMPRESS,
	PGLZ_COMPRESS,
	ZLIB_COMPRESS,
} CompressAlg;

typedef enum ForkName
{
	vm,
	fsm,
	cfm,
	init,
	ptrack
} ForkName;

#define INIT_FILE_CRC32(use_crc32c, crc) \
do { \
	if (use_crc32c) \
		INIT_CRC32C(crc); \
	else \
		INIT_TRADITIONAL_CRC32(crc); \
} while (0)
#define COMP_FILE_CRC32(use_crc32c, crc, data, len) \
do { \
	if (use_crc32c) \
		COMP_CRC32C((crc), (data), (len)); \
	else \
		COMP_TRADITIONAL_CRC32(crc, data, len); \
} while (0)
#define FIN_FILE_CRC32(use_crc32c, crc) \
do { \
	if (use_crc32c) \
		FIN_CRC32C(crc); \
	else \
		FIN_TRADITIONAL_CRC32(crc); \
} while (0)

#define pg_off_t unsigned long long


/* Information about single file (or dir) in backup */
typedef struct pgFile
{
	char   *name;			/* file or directory name */
	mode_t	mode;			/* protection (file type and permission) */
	size_t	size;			/* size of the file */
	time_t  mtime;			/* file st_mtime attribute, can be used only
								during backup */
	size_t	read_size;		/* size of the portion read (if only some pages are
							   backed up, it's different from size) */
	int64	write_size;		/* size of the backed-up file. BYTES_INVALID means
							   that the file existed but was not backed up
							   because not modified since last backup. */
	size_t	uncompressed_size;	/* size of the backed-up file before compression
								 * and adding block headers.
								 */
							/* we need int64 here to store '-1' value */
	pg_crc32 crc;			/* CRC value of the file, regular file only */
	char   *rel_path;		/* relative path of the file */
	char   *linked;			/* path of the linked file */
	bool	is_datafile;	/* true if the file is PostgreSQL data file */
	Oid		tblspcOid;		/* tblspcOid extracted from path, if applicable */
	Oid		dbOid;			/* dbOid extracted from path, if applicable */
	Oid		relOid;			/* relOid extracted from path, if applicable */
	ForkName   forkName;	/* forkName extracted from path, if applicable */
	int		segno;			/* Segment number for ptrack */
	int		n_blocks;		/* number of blocks in the data file in data directory */
	bool	is_cfs;			/* Flag to distinguish files compressed by CFS*/
	int		external_dir_num;	/* Number of external directory. 0 if not external */
	bool	exists_in_prev;		/* Mark files, both data and regular, that exists in previous backup */
	CompressAlg		compress_alg;		/* compression algorithm applied to the file */
	volatile 		pg_atomic_flag lock;/* lock for synchronization of parallel threads  */
	datapagemap_t	pagemap;			/* bitmap of pages updated since previous backup
										   may take up to 16kB per file */
	bool			pagemap_isabsent;	/* Used to mark files with unknown state of pagemap,
										 * i.e. datafiles without _ptrack */
	/* Coordinates in header map */
	int      n_headers;		/* number of blocks in the data file in backup */
	pg_crc32 hdr_crc;		/* CRC value of header file: name_hdr */
	pg_off_t hdr_off;       /* offset in header map */
	int      hdr_size;      /* length of headers */
} pgFile;

typedef struct page_map_entry
{
	const char	*path;		/* file or directory name */
	char		*pagemap;
	size_t		 pagemapsize;
} page_map_entry;

/* Special values of datapagemap_t bitmapsize */
#define PageBitmapIsEmpty 0		/* Used to mark unchanged datafiles */

/* Return codes for check_tablespace_mapping */
#define NoTblspc 0
#define EmptyTblspc 1
#define NotEmptyTblspc 2

/* Current state of backup */
typedef enum BackupStatus
{
	BACKUP_STATUS_INVALID,		/* the pgBackup is invalid */
	BACKUP_STATUS_OK,			/* completed backup */
	BACKUP_STATUS_ERROR,		/* aborted because of unexpected error */
	BACKUP_STATUS_RUNNING,		/* running backup */
	BACKUP_STATUS_MERGING,		/* merging backups */
	BACKUP_STATUS_MERGED,		/* backup has been successfully merged and now awaits
								 * the assignment of new start_time */
	BACKUP_STATUS_DELETING,		/* data files are being deleted */
	BACKUP_STATUS_DELETED,		/* data files have been deleted */
	BACKUP_STATUS_DONE,			/* completed but not validated yet */
	BACKUP_STATUS_ORPHAN,		/* backup validity is unknown but at least one parent backup is corrupted */
	BACKUP_STATUS_CORRUPT		/* files are corrupted, not available */
} BackupStatus;

typedef enum BackupMode
{
	BACKUP_MODE_INVALID = 0,
	BACKUP_MODE_DIFF_PAGE,		/* incremental page backup */
	BACKUP_MODE_DIFF_PTRACK,	/* incremental page backup with ptrack system */
	BACKUP_MODE_DIFF_DELTA,		/* incremental page backup with lsn comparison */
	BACKUP_MODE_FULL			/* full backup */
} BackupMode;

typedef enum ShowFormat
{
	SHOW_PLAIN,
	SHOW_JSON
} ShowFormat;


/* special values of pgBackup fields */
#define INVALID_BACKUP_ID	0    /* backup ID is not provided by user */
#define BYTES_INVALID		(-1) /* file didn`t changed since previous backup, DELTA backup do not rely on it */
#define FILE_NOT_FOUND		(-2) /* file disappeared during backup */
#define BLOCKNUM_INVALID	(-1)
#define PROGRAM_VERSION	"2.5.0"

/* update when remote agent API or behaviour changes */
#define AGENT_PROTOCOL_VERSION 20500
#define AGENT_PROTOCOL_VERSION_STR "2.5.0"

/* update only when changing storage format */
#define STORAGE_FORMAT_VERSION "2.5.0"

typedef struct ConnectionOptions
{
	const char *pgdatabase;
	const char *pghost;
	const char *pgport;
	const char *pguser;
} ConnectionOptions;

typedef struct ConnectionArgs
{
	PGconn	   *conn;
	PGcancel   *cancel_conn;
} ConnectionArgs;

/* Store values for --remote-* option for 'restore_command' constructor */
typedef struct ArchiveOptions
{
	const char *host;
	const char *port;
	const char *user;
} ArchiveOptions;

/*
 * An instance configuration. It can be stored in a configuration file or passed
 * from command line.
 */
typedef struct InstanceConfig
{
	uint64		system_identifier;
	uint32		xlog_seg_size;

	char	   *pgdata;
	char	   *external_dir_str;

	ConnectionOptions conn_opt;
	ConnectionOptions master_conn_opt;

	uint32		replica_timeout; //Deprecated. Not used anywhere

	/* Wait timeout for WAL segment archiving */
	uint32		archive_timeout;

	/* cmdline to be used as restore_command */
	char	   *restore_command;

	/* Logger parameters */
	LoggerConfig logger;

	/* Remote access parameters */
	RemoteConfig remote;

	/* Retention options. 0 disables the option. */
	uint32		retention_redundancy;
	uint32		retention_window;
	uint32		wal_depth;

	CompressAlg	compress_alg;
	int			compress_level;

	/* Archive description */
	ArchiveOptions archive;
} InstanceConfig;

extern ConfigOption instance_options[];
extern InstanceConfig instance_config;
extern time_t current_time;

typedef struct PGNodeInfo
{
	uint32			block_size;
	uint32			wal_block_size;
	uint32			checksum_version;
	bool			is_superuser;
	bool			pgpro_support;

	int				server_version;
	char			server_version_str[100];

	int				ptrack_version_num;
	bool			is_ptrack_enable;
	const char		*ptrack_schema; /* used only for ptrack 2.x */

} PGNodeInfo;

/* structure used for access to block header map */
typedef struct HeaderMap
{
	char     path[MAXPGPATH];
	char     path_tmp[MAXPGPATH]; /* used only in merge */
	FILE    *fp;                  /* used only for writing */
	char    *buf;                 /* buffer */
	pg_off_t offset;              /* current position in fp */
	pthread_mutex_t mutex;

} HeaderMap;

typedef struct pgBackup pgBackup;

/* Information about single backup stored in backup.conf */
struct pgBackup
{
	BackupMode		backup_mode; /* Mode - one of BACKUP_MODE_xxx above*/
	time_t			backup_id;	 /* Identifier of the backup.
								  * Currently it's the same as start_time */
	BackupStatus	status;		/* Status - one of BACKUP_STATUS_xxx above*/
	TimeLineID		tli; 		/* timeline of start and stop backup lsns */
	XLogRecPtr		start_lsn;	/* backup's starting transaction log location */
	XLogRecPtr		stop_lsn;	/* backup's finishing transaction log location */
	time_t			start_time;	/* UTC time of backup creation */
	time_t			merge_dest_backup;	/* start_time of incremental backup with
									 * which this backup is merging with.
									 * Only available for FULL backups
									 * with MERGING or MERGED statuses */
	time_t			merge_time; /* the moment when merge was started or 0 */
	time_t			end_time;	/* the moment when backup was finished, or the moment
								 * when we realized that backup is broken */
	time_t			recovery_time;	/* Earliest moment for which you can restore
									 * the state of the database cluster using
									 * this backup */
	time_t			expire_time;	/* Backup expiration date */
	TransactionId	recovery_xid;	/* Earliest xid for which you can restore
									 * the state of the database cluster using
									 * this backup */
	/*
	 * Amount of raw data. For a full backup, this is the total amount of
	 * data while for a differential backup this is just the difference
	 * of data taken.
	 * BYTES_INVALID means nothing was backed up.
	 */
	int64			data_bytes;
	/* Size of WAL files needed to replay on top of this
	 * backup to reach the consistency.
	 */
	int64			wal_bytes;
	/* Size of data files before applying compression and block header,
	 * WAL files are not included.
	 */
	int64			uncompressed_bytes;

	/* Size of data files in PGDATA at the moment of backup. */
	int64			pgdata_bytes;

	CompressAlg		compress_alg;
	int				compress_level;

	/* Fields needed for compatibility check */
	uint32			block_size;
	uint32			wal_block_size;
	uint32			checksum_version;
	char			program_version[100];
	char			server_version[100];

	bool			stream;			/* Was this backup taken in stream mode?
									 * i.e. does it include all needed WAL files? */
	bool			from_replica;	/* Was this backup taken from replica */
	time_t			parent_backup; 	/* Identifier of the previous backup.
									 * Which is basic backup for this
									 * incremental backup. */
	pgBackup		*parent_backup_link;
	char			*primary_conninfo; /* Connection parameters of the backup
										* in the format suitable for recovery.conf */
	char			*external_dir_str;	/* List of external directories,
										 * separated by ':' */
	char			*root_dir;		/* Full path for root backup directory:
									   backup_path/instance_name/backup_id */
	char			*database_dir;	/* Full path to directory with data files:
									   backup_path/instance_name/backup_id/database */
	parray			*files;			/* list of files belonging to this backup
									 * must be populated explicitly */
	char			*note;

	pg_crc32         content_crc;

	/* map used for access to page headers */
	HeaderMap       hdr_map;
};

/* Recovery target for restore and validate subcommands */
typedef struct pgRecoveryTarget
{
	time_t			target_time;
	/* add one more field in order to avoid deparsing target_time back */
	const char	   *time_string;
	TransactionId	target_xid;
	/* add one more field in order to avoid deparsing target_xid back */
	const char	   *xid_string;
	XLogRecPtr		target_lsn;
	/* add one more field in order to avoid deparsing target_lsn back */
	const char	   *lsn_string;
	TimeLineID		target_tli;
	bool			target_inclusive;
	bool			inclusive_specified;
	const char	   *target_stop;
	const char	   *target_name;
	const char	   *target_action;
} pgRecoveryTarget;

/* Options needed for restore and validate commands */
typedef struct pgRestoreParams
{
	bool	force;
	bool	is_restore;
	bool	no_validate;
	bool	restore_as_replica;
	//TODO maybe somehow add restore_as_replica as one of RecoverySettingsModes
	RecoverySettingsMode recovery_settings_mode;
	bool	skip_external_dirs;
	bool	skip_block_validation; //Start using it
	const char *restore_command;
	const char *primary_slot_name;
	const char *primary_conninfo;

	/* options for incremental restore */
	IncrRestoreMode	incremental_mode;
	XLogRecPtr shift_lsn;

	/* options for partial restore */
	PartialRestoreType partial_restore_type;
	parray *partial_db_list;
} pgRestoreParams;

/* Options needed for set-backup command */
typedef struct pgSetBackupParams
{
	int64   ttl; /* amount of time backup must be pinned
				  * -1 - do nothing
				  * 0 - disable pinning
				  */
	time_t  expire_time; /* Point in time until backup
						  * must be pinned.
						  */
	char   *note;
} pgSetBackupParams;

typedef struct
{
	PGNodeInfo *nodeInfo;

	const char *from_root;
	const char *to_root;
	const char *external_prefix;

	parray	   *files_list;
	parray	   *prev_filelist;
	parray	   *external_dirs;
	XLogRecPtr	prev_start_lsn;

	int			thread_num;
	HeaderMap   *hdr_map;

	/*
	 * Return value from the thread.
	 * 0 means there is no error, 1 - there is an error.
	 */
	int			ret;
} backup_files_arg;

typedef struct timelineInfo timelineInfo;

/* struct to collect info about timelines in WAL archive */
struct timelineInfo {

	TimeLineID tli;			/* this timeline */
	TimeLineID parent_tli;  /* parent timeline. 0 if none */
	timelineInfo *parent_link; /* link to parent timeline */
	XLogRecPtr switchpoint;	   /* if this timeline has a parent, then
								* switchpoint contains switchpoint LSN,
								* otherwise 0 */
	XLogSegNo begin_segno;	/* first present segment in this timeline */
	XLogSegNo end_segno;	/* last present segment in this timeline */
	size_t	n_xlog_files;	/* number of segments (only really existing)
							 * does not include lost segments */
	size_t	size;			/* space on disk taken by regular WAL files */
	parray *backups;		/* array of pgBackup sturctures with info
							 * about backups belonging to this timeline */
	parray *xlog_filelist;	/* array of ordinary WAL segments, '.partial'
							 * and '.backup' files belonging to this timeline */
	parray *lost_segments;	/* array of intervals of lost segments */
	parray *keep_segments;	/* array of intervals of segments used by WAL retention */
	pgBackup *closest_backup; /* link to valid backup, closest to timeline */
	pgBackup *oldest_backup; /* link to oldest backup on timeline */
	XLogRecPtr anchor_lsn; /* LSN belonging to the oldest segno to keep for 'wal-depth' */
	TimeLineID anchor_tli;	/* timeline of anchor_lsn */
};

typedef struct xlogInterval
{
	XLogSegNo begin_segno;
	XLogSegNo end_segno;
} xlogInterval;

typedef struct lsnInterval
{
	TimeLineID tli;
	XLogRecPtr begin_lsn;
	XLogRecPtr end_lsn;
} lsnInterval;

typedef enum xlogFileType
{
	SEGMENT,
	TEMP_SEGMENT,
	PARTIAL_SEGMENT,
	BACKUP_HISTORY_FILE
} xlogFileType;

typedef struct xlogFile
{
	pgFile       file;
	XLogSegNo    segno;
	xlogFileType type;
	bool         keep; /* Used to prevent removal of WAL segments
                        * required by ARCHIVE backups. */
} xlogFile;


/*
 * When copying datafiles to backup we validate and compress them block
 * by block. Thus special header is required for each data block.
 */
typedef struct BackupPageHeader
{
	BlockNumber	block;			/* block number */
	int32		compressed_size;
} BackupPageHeader;

/* 4MB for 1GB file */
typedef struct BackupPageHeader2
{
	XLogRecPtr  lsn;
	int32	    block;			 /* block number */
	int32       pos;             /* position in backup file */
	uint16      checksum;
} BackupPageHeader2;

typedef struct StopBackupCallbackParams
{
	PGconn	*conn;
	int      server_version;
} StopBackupCallbackParams;

/* Special value for compressed_size field */
#define PageIsOk		 0
#define SkipCurrentPage -1
#define PageIsTruncated -2
#define PageIsCorrupted -3 /* used by checkdb */

/*
 * Return timeline, xlog ID and record offset from an LSN of the type
 * 0/B000188, usual result from pg_stop_backup() and friends.
 */
#define XLogDataFromLSN(data, xlogid, xrecoff)		\
	sscanf(data, "%X/%X", xlogid, xrecoff)

#define IsCompressedXLogFileName(fname) \
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".gz") &&			\
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".gz") == 0)

#if PG_VERSION_NUM >= 110000

#define WalSegmentOffset(xlogptr, wal_segsz_bytes) \
	XLogSegmentOffset(xlogptr, wal_segsz_bytes)
#define GetXLogSegNo(xlrp, logSegNo, wal_segsz_bytes) \
	XLByteToSeg(xlrp, logSegNo, wal_segsz_bytes)
#define GetXLogRecPtr(segno, offset, wal_segsz_bytes, dest) \
	XLogSegNoOffsetToRecPtr(segno, offset, wal_segsz_bytes, dest)
#define GetXLogFileName(fname, tli, logSegNo, wal_segsz_bytes) \
	XLogFileName(fname, tli, logSegNo, wal_segsz_bytes)
#define IsInXLogSeg(xlrp, logSegNo, wal_segsz_bytes) \
	XLByteInSeg(xlrp, logSegNo, wal_segsz_bytes)
#define GetXLogSegName(fname, logSegNo, wal_segsz_bytes)	\
	snprintf(fname, 20, "%08X%08X",		\
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)))

#define GetXLogSegNoFromScrath(logSegNo, log, seg, wal_segsz_bytes)	\
		logSegNo = (uint64) log * XLogSegmentsPerXLogId(wal_segsz_bytes) + seg

#define GetXLogFromFileName(fname, tli, logSegNo, wal_segsz_bytes) \
		XLogFromFileName(fname, tli, logSegNo, wal_segsz_bytes)
#else
#define WalSegmentOffset(xlogptr, wal_segsz_bytes) \
	((xlogptr) & ((XLogSegSize) - 1))
#define GetXLogSegNo(xlrp, logSegNo, wal_segsz_bytes) \
	XLByteToSeg(xlrp, logSegNo)
#define GetXLogRecPtr(segno, offset, wal_segsz_bytes, dest) \
	XLogSegNoOffsetToRecPtr(segno, offset, dest)
#define GetXLogFileName(fname, tli, logSegNo, wal_segsz_bytes) \
	XLogFileName(fname, tli, logSegNo)
#define IsInXLogSeg(xlrp, logSegNo, wal_segsz_bytes) \
	XLByteInSeg(xlrp, logSegNo)
#define GetXLogSegName(fname, logSegNo, wal_segsz_bytes) \
	snprintf(fname, 20, "%08X%08X",\
			 (uint32) ((logSegNo) / XLogSegmentsPerXLogId), \
			 (uint32) ((logSegNo) % XLogSegmentsPerXLogId))

#define GetXLogSegNoFromScrath(logSegNo, log, seg, wal_segsz_bytes)	\
		logSegNo = (uint64) log * XLogSegmentsPerXLogId + seg

#define GetXLogFromFileName(fname, tli, logSegNo, wal_segsz_bytes) \
		XLogFromFileName(fname, tli, logSegNo)
#endif

#define IsPartialCompressXLogFileName(fname)	\
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".gz.partial") && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".gz.partial") == 0)

#define IsTempXLogFileName(fname)	\
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".part") &&	\
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".part") == 0)

#define IsTempCompressXLogFileName(fname)	\
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".gz.part") && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".gz.part") == 0)

#define IsSshProtocol() (instance_config.remote.host && strcmp(instance_config.remote.proto, "ssh") == 0)

/* common options */
extern pid_t    my_pid;
extern __thread int my_thread_num;
extern int		num_threads;
extern bool		stream_wal;
extern bool		show_color;
extern bool		progress;
extern bool     is_archive_cmd; /* true for archive-{get,push} */
#if PG_VERSION_NUM >= 100000
/* In pre-10 'replication_slot' is defined in receivelog.h */
extern char	   *replication_slot;
#endif
extern bool 	temp_slot;

/* backup options */
extern bool		smooth_checkpoint;

/* remote probackup options */
extern char* remote_agent;

extern bool exclusive_backup;

/* delete options */
extern bool		delete_wal;
extern bool		delete_expired;
extern bool		merge_expired;
extern bool		dry_run;

/* ===== instanceState ===== */

typedef struct InstanceState
{
	/* catalog, this instance belongs to */
	CatalogState *catalog_state;

	char		instance_name[MAXPGPATH]; //previously global var instance_name
	/* $BACKUP_PATH/backups/instance_name */
	char		instance_backup_subdir_path[MAXPGPATH];

	/* $BACKUP_PATH/backups/instance_name/BACKUP_CATALOG_CONF_FILE */
	char		instance_config_path[MAXPGPATH];
	
	/* $BACKUP_PATH/backups/instance_name */
	char		instance_wal_subdir_path[MAXPGPATH]; // previously global var arclog_path

	/* TODO: Make it more specific */
	PGconn *conn;


	//TODO split into some more meaningdul parts
    InstanceConfig *config;
} InstanceState;

/* ===== instanceState (END) ===== */

/* show options */
extern ShowFormat show_format;

/* checkdb options */
extern bool heapallindexed;
extern bool skip_block_validation;

/* current settings */
extern pgBackup current;

/* argv of the process */
extern char** commands_args;

/* in backup.c */
extern int do_backup(InstanceState *instanceState, pgSetBackupParams *set_backup_params,
					 bool no_validate, bool no_sync, bool backup_logs);
extern void do_checkdb(bool need_amcheck, ConnectionOptions conn_opt,
				  char *pgdata);
extern BackupMode parse_backup_mode(const char *value);
extern const char *deparse_backup_mode(BackupMode mode);
extern void process_block_change(ForkNumber forknum, RelFileNode rnode,
								 BlockNumber blkno);

/* in restore.c */
extern int do_restore_or_validate(InstanceState *instanceState,
					  time_t target_backup_id,
					  pgRecoveryTarget *rt,
					  pgRestoreParams *params,
					  bool no_sync);
extern bool satisfy_timeline(const parray *timelines, const pgBackup *backup);
extern bool satisfy_recovery_target(const pgBackup *backup,
									const pgRecoveryTarget *rt);
extern pgRecoveryTarget *parseRecoveryTargetOptions(
	const char *target_time, const char *target_xid,
	const char *target_inclusive, TimeLineID target_tli, const char* target_lsn,
	const char *target_stop, const char *target_name,
	const char *target_action);

extern parray *get_dbOid_exclude_list(pgBackup *backup, parray *datname_list,
										PartialRestoreType partial_restore_type);

extern parray *get_backup_filelist(pgBackup *backup, bool strict);
extern parray *read_timeline_history(const char *arclog_path, TimeLineID targetTLI, bool strict);
extern bool tliIsPartOfHistory(const parray *timelines, TimeLineID tli);

/* in merge.c */
extern void do_merge(InstanceState *instanceState, time_t backup_id, bool no_validate, bool no_sync);
extern void merge_backups(pgBackup *backup, pgBackup *next_backup);
extern void merge_chain(InstanceState *instanceState, parray *parent_chain,
						pgBackup *full_backup, pgBackup *dest_backup,
						bool no_validate, bool no_sync);

extern parray *read_database_map(pgBackup *backup);

/* in init.c */
extern int do_init(CatalogState *catalogState);
extern int do_add_instance(InstanceState *instanceState, InstanceConfig *instance);

/* in archive.c */
extern void do_archive_push(InstanceState *instanceState, InstanceConfig *instance, char *wal_file_path,
						   char *wal_file_name, int batch_size, bool overwrite,
						   bool no_sync, bool no_ready_rename);
extern void do_archive_get(InstanceState *instanceState, InstanceConfig *instance, const char *prefetch_dir_arg, char *wal_file_path,
						   char *wal_file_name, int batch_size, bool validate_wal);

/* in configure.c */
extern void do_show_config(void);
extern void do_set_config(InstanceState *instanceState, bool missing_ok);
extern void init_config(InstanceConfig *config, const char *instance_name);
extern InstanceConfig *readInstanceConfigFile(InstanceState *instanceState);

/* in show.c */
extern int do_show(CatalogState *catalogState, InstanceState *instanceState,
				   time_t requested_backup_id, bool show_archive);

/* in delete.c */
extern void do_delete(InstanceState *instanceState, time_t backup_id);
extern void delete_backup_files(pgBackup *backup);
extern void do_retention(InstanceState *instanceState, bool no_validate, bool no_sync);
extern int do_delete_instance(InstanceState *instanceState);
extern void do_delete_status(InstanceState *instanceState, 
					InstanceConfig *instance_config, const char *status);

/* in fetch.c */
extern char *slurpFile(const char *datadir,
					   const char *path,
					   size_t *filesize,
					   bool safe,
					   fio_location location);
extern char *fetchFile(PGconn *conn, const char *filename, size_t *filesize);

/* in help.c */
extern void help_print_version(void);
extern void help_pg_probackup(void);
extern void help_command(ProbackupSubcmd const subcmd);

/* in validate.c */
extern void pgBackupValidate(pgBackup* backup, pgRestoreParams *params);
extern int do_validate_all(CatalogState *catalogState, InstanceState *instanceState);
extern int validate_one_page(Page page, BlockNumber absolute_blkno,
							 XLogRecPtr stop_lsn, PageState *page_st,
							 uint32 checksum_version);
extern bool validate_tablespace_map(pgBackup *backup, bool no_validate);

extern parray* get_history_streaming(ConnectionOptions *conn_opt, TimeLineID tli, parray *backup_list);

/* return codes for validate_one_page */
/* TODO: use enum */
#define PAGE_IS_VALID (-1)
#define PAGE_IS_NOT_FOUND (-2)
#define PAGE_IS_ZEROED (-3)
#define PAGE_HEADER_IS_INVALID (-4)
#define PAGE_CHECKSUM_MISMATCH (-5)
#define PAGE_LSN_FROM_FUTURE (-6)

/* in catalog.c */
extern pgBackup *read_backup(const char *root_dir);
extern void write_backup(pgBackup *backup, bool strict);
extern void write_backup_status(pgBackup *backup, BackupStatus status,
								bool strict);
extern void write_backup_data_bytes(pgBackup *backup);
extern bool lock_backup(pgBackup *backup, bool strict, bool exclusive);

extern const char *pgBackupGetBackupMode(pgBackup *backup, bool show_color);
extern void pgBackupGetBackupModeColor(pgBackup *backup, char *mode);

extern parray *catalog_get_instance_list(CatalogState *catalogState);

extern parray *catalog_get_backup_list(InstanceState *instanceState, time_t requested_backup_id);
extern void catalog_lock_backup_list(parray *backup_list, int from_idx,
									 int to_idx, bool strict, bool exclusive);
extern pgBackup *catalog_get_last_data_backup(parray *backup_list,
											  TimeLineID tli,
											  time_t current_start_time);
extern pgBackup *get_multi_timeline_parent(parray *backup_list, parray *tli_list,
	                      TimeLineID current_tli, time_t current_start_time,
						  InstanceConfig *instance);
extern timelineInfo *timelineInfoNew(TimeLineID tli);
extern void timelineInfoFree(void *tliInfo);
extern parray *catalog_get_timelines(InstanceState *instanceState, InstanceConfig *instance);
extern void do_set_backup(InstanceState *instanceState, time_t backup_id,
							pgSetBackupParams *set_backup_params);
extern void pin_backup(pgBackup	*target_backup,
							pgSetBackupParams *set_backup_params);
extern void add_note(pgBackup *target_backup, char *note);
extern void pgBackupWriteControl(FILE *out, pgBackup *backup, bool utc);
extern void write_backup_filelist(pgBackup *backup, parray *files,
								  const char *root, parray *external_list, bool sync);


extern void pgBackupCreateDir(pgBackup *backup, const char *backup_instance_path);
extern void pgNodeInit(PGNodeInfo *node);
extern void pgBackupInit(pgBackup *backup);
extern void pgBackupFree(void *backup);
extern int pgBackupCompareId(const void *f1, const void *f2);
extern int pgBackupCompareIdDesc(const void *f1, const void *f2);
extern int pgBackupCompareIdEqual(const void *l, const void *r);

extern pgBackup* find_parent_full_backup(pgBackup *current_backup);
extern int scan_parent_chain(pgBackup *current_backup, pgBackup **result_backup);
/* return codes for scan_parent_chain */
#define ChainIsBroken 0
#define ChainIsInvalid 1
#define ChainIsOk 2

extern bool is_parent(time_t parent_backup_time, pgBackup *child_backup, bool inclusive);
extern bool is_prolific(parray *backup_list, pgBackup *target_backup);
extern void append_children(parray *backup_list, pgBackup *target_backup, parray *append_list);
extern bool launch_agent(void);
extern void launch_ssh(char* argv[]);
extern void wait_ssh(void);

#define COMPRESS_ALG_DEFAULT NOT_DEFINED_COMPRESS
#define COMPRESS_LEVEL_DEFAULT 1

extern CompressAlg parse_compress_alg(const char *arg);
extern const char* deparse_compress_alg(int alg);

/* in dir.c */
extern bool get_control_value(const char *str, const char *name,
				  char *value_str, int64 *value_int64, bool is_mandatory);
extern void dir_list_file(parray *files, const char *root, bool exclude,
						  bool follow_symlink, bool add_root, bool backup_logs,
						  bool skip_hidden, int external_dir_num, fio_location location);

extern void create_data_directories(parray *dest_files,
										const char *data_dir,
										const char *backup_dir,
										bool extract_tablespaces,
										bool incremental,
										fio_location location);

extern void read_tablespace_map(parray *links, const char *backup_dir);
extern void opt_tablespace_map(ConfigOption *opt, const char *arg);
extern void opt_externaldir_map(ConfigOption *opt, const char *arg);
extern int  check_tablespace_mapping(pgBackup *backup, bool incremental, bool force, bool pgdata_is_empty, bool no_validate);
extern void check_external_dir_mapping(pgBackup *backup, bool incremental);
extern char *get_external_remap(char *current_dir);

extern void print_database_map(FILE *out, parray *database_list);
extern void write_database_map(pgBackup *backup, parray *database_list,
								   parray *backup_file_list);
extern void db_map_entry_free(void *map);

extern void print_file_list(FILE *out, const parray *files, const char *root,
							const char *external_prefix, parray *external_list);
extern parray *make_external_directory_list(const char *colon_separated_dirs,
											bool remap);
extern void free_dir_list(parray *list);
extern void makeExternalDirPathByNum(char *ret_path, const char *pattern_path,
									 const int dir_num);
extern bool backup_contains_external(const char *dir, parray *dirs_list);

extern int dir_create_dir(const char *path, mode_t mode, bool strict);
extern bool dir_is_empty(const char *path, fio_location location);

extern bool fileExists(const char *path, fio_location location);
extern size_t pgFileSize(const char *path);

extern pgFile *pgFileNew(const char *path, const char *rel_path,
						 bool follow_symlink, int external_dir_num,
						 fio_location location);
extern pgFile *pgFileInit(const char *rel_path);
extern void pgFileDelete(mode_t mode, const char *full_path);
extern void fio_pgFileDelete(pgFile *file, const char *full_path);

extern void pgFileFree(void *file);

extern pg_crc32 pgFileGetCRC(const char *file_path, bool use_crc32c, bool missing_ok);
extern pg_crc32 pgFileGetCRCgz(const char *file_path, bool use_crc32c, bool missing_ok);

extern int pgFileMapComparePath(const void *f1, const void *f2);
extern int pgFileCompareName(const void *f1, const void *f2);
extern int pgFileCompareRelPathWithExternal(const void *f1, const void *f2);
extern int pgFileCompareRelPathWithExternalDesc(const void *f1, const void *f2);
extern int pgFileCompareLinked(const void *f1, const void *f2);
extern int pgFileCompareSize(const void *f1, const void *f2);
extern int pgCompareOid(const void *f1, const void *f2);
extern void pfilearray_clear_locks(parray *file_list);

/* in data.c */
extern bool check_data_file(ConnectionArgs *arguments, pgFile *file,
							const char *from_fullpath, uint32 checksum_version);

extern void backup_data_file(pgFile *file, const char *from_fullpath, const char *to_fullpath,
							 XLogRecPtr prev_backup_start_lsn, BackupMode backup_mode,
							 CompressAlg calg, int clevel, uint32 checksum_version,
							 int ptrack_version_num, const char *ptrack_schema,
							 HeaderMap *hdr_map, bool missing_ok);
extern void backup_non_data_file(pgFile *file, pgFile *prev_file,
								 const char *from_fullpath, const char *to_fullpath,
								 BackupMode backup_mode, time_t parent_backup_time,
								 bool missing_ok);
extern void backup_non_data_file_internal(const char *from_fullpath,
										  fio_location from_location,
										  const char *to_fullpath, pgFile *file,
										  bool missing_ok);

extern size_t restore_data_file(parray *parent_chain, pgFile *dest_file, FILE *out,
								const char *to_fullpath, bool use_bitmap, PageState *checksum_map,
								XLogRecPtr shift_lsn, datapagemap_t *lsn_map, bool use_headers);
extern size_t restore_data_file_internal(FILE *in, FILE *out, pgFile *file, uint32 backup_version,
										 const char *from_fullpath, const char *to_fullpath, int nblocks,
										 datapagemap_t *map, PageState *checksum_map, int checksum_version,
										 datapagemap_t *lsn_map, BackupPageHeader2 *headers);
extern size_t restore_non_data_file(parray *parent_chain, pgBackup *dest_backup,
									pgFile *dest_file, FILE *out, const char *to_fullpath,
									bool already_exists);
extern void restore_non_data_file_internal(FILE *in, FILE *out, pgFile *file,
										   const char *from_fullpath, const char *to_fullpath);
extern bool create_empty_file(fio_location from_location, const char *to_root,
							  fio_location to_location, pgFile *file);

extern PageState *get_checksum_map(const char *fullpath, uint32 checksum_version,
								int n_blocks, XLogRecPtr dest_stop_lsn, BlockNumber segmentno);
extern datapagemap_t *get_lsn_map(const char *fullpath, uint32 checksum_version,
								  int n_blocks, XLogRecPtr shift_lsn, BlockNumber segmentno);
extern bool validate_file_pages(pgFile *file, const char *fullpath, XLogRecPtr stop_lsn,
							    uint32 checksum_version, uint32 backup_version, HeaderMap *hdr_map);

extern BackupPageHeader2* get_data_file_headers(HeaderMap *hdr_map, pgFile *file, uint32 backup_version, bool strict);
extern void write_page_headers(BackupPageHeader2 *headers, pgFile *file, HeaderMap *hdr_map, bool is_merge);
extern void init_header_map(pgBackup *backup);
extern void cleanup_header_map(HeaderMap *hdr_map);
/* parsexlog.c */
extern bool extractPageMap(const char *archivedir, uint32 wal_seg_size,
						   XLogRecPtr startpoint, TimeLineID start_tli,
						   XLogRecPtr endpoint, TimeLineID end_tli,
						   parray *tli_list);
extern void validate_wal(pgBackup *backup, const char *archivedir,
						 time_t target_time, TransactionId target_xid,
						 XLogRecPtr target_lsn, TimeLineID tli,
						 uint32 seg_size);
extern bool validate_wal_segment(TimeLineID tli, XLogSegNo segno,
								 const char *prefetch_dir, uint32 wal_seg_size);
extern bool read_recovery_info(const char *archivedir, TimeLineID tli,
							   uint32 seg_size,
							   XLogRecPtr start_lsn, XLogRecPtr stop_lsn,
							   time_t *recovery_time);
extern bool wal_contains_lsn(const char *archivedir, XLogRecPtr target_lsn,
							 TimeLineID target_tli, uint32 seg_size);
extern XLogRecPtr get_prior_record_lsn(const char *archivedir, XLogRecPtr start_lsn,
								   XLogRecPtr stop_lsn, TimeLineID tli,
								   bool seek_prev_segment, uint32 seg_size);

extern XLogRecPtr get_first_record_lsn(const char *archivedir, XLogRecPtr start_lsn,
									   TimeLineID tli, uint32 wal_seg_size, int timeout);
extern XLogRecPtr get_next_record_lsn(const char *archivedir, XLogSegNo	segno, TimeLineID tli,
									  uint32 wal_seg_size, int timeout, XLogRecPtr target);

/* in util.c */
extern TimeLineID get_current_timeline(PGconn *conn);
extern TimeLineID get_current_timeline_from_control(bool safe);
extern XLogRecPtr get_checkpoint_location(PGconn *conn);
extern uint64 get_system_identifier(const char *pgdata_path, const char *pg_control_filename, fio_location location);
extern uint64 get_remote_system_identifier(PGconn *conn);
extern uint32 get_data_checksum_version(bool safe);
extern pg_crc32c get_pgcontrol_checksum(const char *pgdata_path);
extern uint32 get_xlog_seg_size(char *pgdata_path);
extern void get_redo(const char *pgdata_path, const char *pg_control_filename, RedoParams *redo);
extern void set_min_recovery_point(pgFile *file, const char *backup_path,
								   XLogRecPtr stop_backup_lsn);
extern void copy_pgcontrol_file(const char *from_fullpath, fio_location from_location,
					const char *to_fullpath, fio_location to_location, pgFile *file);

extern void time2iso(char *buf, size_t len, time_t time, bool utc);
extern const char *status2str(BackupStatus status);
const char *status2str_color(BackupStatus status);
extern BackupStatus str2status(const char *status);
extern const char *base36enc(long unsigned int value);
extern char *base36enc_dup(long unsigned int value);
extern long unsigned int base36dec(const char *text);
extern uint32 parse_server_version(const char *server_version_str);
extern uint32 parse_program_version(const char *program_version);
extern bool   parse_page(Page page, XLogRecPtr *lsn);
extern int32  do_compress(void* dst, size_t dst_size, void const* src, size_t src_size,
						  CompressAlg alg, int level, const char **errormsg);
extern int32  do_decompress(void* dst, size_t dst_size, void const* src, size_t src_size,
							CompressAlg alg, const char **errormsg);

extern void pretty_size(int64 size, char *buf, size_t len);
extern void pretty_time_interval(double time, char *buf, size_t len);

extern PGconn *pgdata_basic_setup(ConnectionOptions conn_opt, PGNodeInfo *nodeInfo);
extern void check_system_identifiers(PGconn *conn, char *pgdata);
extern void parse_filelist_filenames(parray *files, const char *root);

/* in ptrack.c */
extern void make_pagemap_from_ptrack_2(parray* files, PGconn* backup_conn,
									   const char *ptrack_schema,
									   int ptrack_version_num,
									   XLogRecPtr lsn);
extern void get_ptrack_version(PGconn *backup_conn, PGNodeInfo *nodeInfo);
extern bool pg_ptrack_enable(PGconn *backup_conn, int ptrack_version_num);
extern XLogRecPtr get_last_ptrack_lsn(PGconn *backup_conn, PGNodeInfo *nodeInfo);
extern parray * pg_ptrack_get_pagemapset(PGconn *backup_conn, const char *ptrack_schema,
										 int ptrack_version_num, XLogRecPtr lsn);

/* open local file to writing */
extern FILE* open_local_file_rw(const char *to_fullpath, char **out_buf, uint32 buf_size);

extern int send_pages(const char *to_fullpath, const char *from_fullpath,
					  pgFile *file, XLogRecPtr prev_backup_start_lsn, CompressAlg calg, int clevel,
					  uint32 checksum_version, bool use_pagemap, BackupPageHeader2 **headers,
					  BackupMode backup_mode, int ptrack_version_num, const char *ptrack_schema);

/* FIO */
extern void setMyLocation(ProbackupSubcmd const subcmd);
extern void fio_delete(mode_t mode, const char *fullpath, fio_location location);
extern int fio_send_pages(const char *to_fullpath, const char *from_fullpath, pgFile *file,
	                      XLogRecPtr horizonLsn, int calg, int clevel, uint32 checksum_version,
	                      bool use_pagemap, BlockNumber *err_blknum, char **errormsg,
	                      BackupPageHeader2 **headers);
/* return codes for fio_send_pages */
extern int fio_send_file_gz(const char *from_fullpath, const char *to_fullpath, FILE* out, char **errormsg);
extern int fio_send_file(const char *from_fullpath, const char *to_fullpath, FILE* out,
														pgFile *file, char **errormsg);

extern void fio_list_dir(parray *files, const char *root, bool exclude, bool follow_symlink,
						 bool add_root, bool backup_logs, bool skip_hidden, int external_dir_num);

extern bool pgut_rmtree(const char *path, bool rmtopdir, bool strict);

extern void pgut_setenv(const char *key, const char *val);
extern void pgut_unsetenv(const char *key);

extern PageState *fio_get_checksum_map(const char *fullpath, uint32 checksum_version, int n_blocks,
									XLogRecPtr dest_stop_lsn, BlockNumber segmentno, fio_location location);

extern datapagemap_t *fio_get_lsn_map(const char *fullpath, uint32 checksum_version,
							int n_blocks, XLogRecPtr horizonLsn, BlockNumber segmentno,
							fio_location location);
extern pid_t fio_check_postmaster(const char *pgdata, fio_location location);

extern int32 fio_decompress(void* dst, void const* src, size_t size, int compress_alg, char **errormsg);

/* return codes for fio_send_pages() and fio_send_file() */
#define SEND_OK       (0)
#define FILE_MISSING (-1)
#define OPEN_FAILED  (-2)
#define READ_FAILED  (-3)
#define WRITE_FAILED (-4)
#define ZLIB_ERROR   (-5)
#define REMOTE_ERROR (-6)
#define PAGE_CORRUPTION (-8)

/* Check if specified location is local for current node */
extern bool fio_is_remote(fio_location location);
extern bool fio_is_remote_simple(fio_location location);

extern void get_header_errormsg(Page page, char **errormsg);
extern void get_checksum_errormsg(Page page, char **errormsg,
								  BlockNumber absolute_blkno);

extern bool
datapagemap_is_set(datapagemap_t *map, BlockNumber blkno);

extern void
datapagemap_print_debug(datapagemap_t *map);

/* in stream.c */
extern XLogRecPtr stop_backup_lsn;
extern void start_WAL_streaming(PGconn *backup_conn, char *stream_dst_path,
							   ConnectionOptions *conn_opt,
							   XLogRecPtr startpos, TimeLineID starttli);
extern int wait_WAL_streaming_end(parray *backup_files_list);

/* external variables and functions, implemented in backup.c */
typedef struct PGStopBackupResult
{
	/*
	 * We will use values of snapshot_xid and invocation_time if there are
	 * no transactions between start_lsn and stop_lsn.
	 */
	TransactionId	snapshot_xid;
	time_t		invocation_time;
	/*
	 * Fields that store pg_catalog.pg_stop_backup() result
	 */
	XLogRecPtr	lsn;
	size_t		backup_label_content_len;
	char		*backup_label_content;
	size_t		tablespace_map_content_len;
	char		*tablespace_map_content;
} PGStopBackupResult;

extern bool backup_in_progress;
extern parray *backup_files_list;

extern void pg_start_backup(const char *label, bool smooth, pgBackup *backup,
							PGNodeInfo *nodeInfo, PGconn *conn);
extern void pg_silent_client_messages(PGconn *conn);
extern void pg_create_restore_point(PGconn *conn, time_t backup_start_time);
extern void pg_stop_backup_send(PGconn *conn, int server_version, bool is_started_on_replica, bool is_exclusive, char **query_text);
extern void pg_stop_backup_consume(PGconn *conn, int server_version,
		bool is_exclusive, uint32 timeout, const char *query_text,
		PGStopBackupResult *result);
extern void pg_stop_backup_write_file_helper(const char *path, const char *filename, const char *error_msg_filename,
		const void *data, size_t len, parray *file_list);
extern XLogRecPtr wait_wal_lsn(const char *wal_segment_dir, XLogRecPtr lsn, bool is_start_lsn, TimeLineID tli,
								bool in_prev_segment, bool segment_only,
								int timeout_elevel, bool in_stream_dir);
extern void wait_wal_and_calculate_stop_lsn(const char *xlog_path, XLogRecPtr stop_lsn, pgBackup *backup);

#endif /* PG_PROBACKUP_H */
