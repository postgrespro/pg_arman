# you need os for unittest to work
import os
import gc
from sys import exit, argv, version_info
import subprocess
import shutil
import six
import testgres
import hashlib
import re
import getpass
import select
from time import sleep
import re
import json

idx_ptrack = {
    't_heap': {
        'type': 'heap'
    },
    't_btree': {
        'type': 'btree',
        'column': 'text',
        'relation': 't_heap'
    },
    't_seq': {
        'type': 'seq',
        'column': 't_seq',
        'relation': 't_heap'
    },
    't_spgist': {
        'type': 'spgist',
        'column': 'text',
        'relation': 't_heap'
    },
    't_brin': {
        'type': 'brin',
        'column': 'text',
        'relation': 't_heap'
    },
    't_gist': {
        'type': 'gist',
        'column': 'tsvector',
        'relation': 't_heap'
    },
    't_gin': {
        'type': 'gin',
        'column': 'tsvector',
        'relation': 't_heap'
    },
    't_hash': {
        'type': 'hash',
        'column': 'id',
        'relation': 't_heap'
    },
    't_bloom': {
        'type': 'bloom',
        'column': 'id',
        'relation': 't_heap'
    }
}

warning = """
Wrong splint in show_pb
Original Header:
{header}
Original Body:
{body}
Splitted Header
{header_split}
Splitted Body
{body_split}
"""


def dir_files(base_dir):
    out_list = []
    for dir_name, subdir_list, file_list in os.walk(base_dir):
        if dir_name != base_dir:
            out_list.append(os.path.relpath(dir_name, base_dir))
        for fname in file_list:
            out_list.append(
                os.path.relpath(os.path.join(
                    dir_name, fname), base_dir)
                )
    out_list.sort()
    return out_list


def is_enterprise():
    # pg_config --help
    if os.name == 'posix':
        cmd = [os.environ['PG_CONFIG'], '--help']

    elif os.name == 'nt':
        cmd = [[os.environ['PG_CONFIG']], ['--help']]

    p = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    if b'postgrespro.ru' in p.communicate()[0]:
        return True
    else:
        return False


class ProbackupException(Exception):
    def __init__(self, message, cmd):
        self.message = message
        self.cmd = cmd

    def __str__(self):
        return '\n ERROR: {0}\n CMD: {1}'.format(repr(self.message), self.cmd)


def slow_start(self, replica=False):

    # wait for https://github.com/postgrespro/testgres/pull/50
#    self.start()
#    self.poll_query_until(
#       "postgres",
#       "SELECT not pg_is_in_recovery()",
#       suppress={testgres.NodeConnection})
    if replica:
        query = 'SELECT pg_is_in_recovery()'
    else:
        query = 'SELECT not pg_is_in_recovery()'

    self.start()
    while True:
        try:
            output = self.safe_psql('template1', query).decode("utf-8").rstrip()

            if output == 't':
                break

        except testgres.QueryException as e:
            if 'database system is starting up' in e.message:
                pass
            elif 'FATAL:  the database system is not accepting connections' in e.message:
                pass
            elif replica and 'Hot standby mode is disabled' in e.message:
                raise e
            else:
                raise e

        sleep(0.5)

class ProbackupTest(object):
    # Class attributes
    enterprise = is_enterprise()

    def __init__(self, *args, **kwargs):
        super(ProbackupTest, self).__init__(*args, **kwargs)
        if '-v' in argv or '--verbose' in argv:
            self.verbose = True
        else:
            self.verbose = False

        self.test_env = os.environ.copy()
        envs_list = [
            'LANGUAGE',
            'LC_ALL',
            'PGCONNECT_TIMEOUT',
            'PGDATA',
            'PGDATABASE',
            'PGHOSTADDR',
            'PGREQUIRESSL',
            'PGSERVICE',
            'PGSSLMODE',
            'PGUSER',
            'PGPORT',
            'PGHOST'
        ]

        for e in envs_list:
            try:
                del self.test_env[e]
            except:
                pass

        self.test_env['LC_MESSAGES'] = 'C'
        self.test_env['LC_TIME'] = 'C'

        self.paranoia = False
        if 'PG_PROBACKUP_PARANOIA' in self.test_env:
            if self.test_env['PG_PROBACKUP_PARANOIA'] == 'ON':
                self.paranoia = True

        self.archive_compress = False
        if 'ARCHIVE_COMPRESSION' in self.test_env:
            if self.test_env['ARCHIVE_COMPRESSION'] == 'ON':
                self.archive_compress = True
        try:
            testgres.configure_testgres(
                cache_initdb=False,
                cached_initdb_dir=False,
                cache_pg_config=False,
                node_cleanup_full=False)
        except:
            pass

        self.helpers_path = os.path.dirname(os.path.realpath(__file__))
        self.dir_path = os.path.abspath(
            os.path.join(self.helpers_path, os.pardir)
            )
        self.tmp_path = os.path.abspath(
            os.path.join(self.dir_path, 'tmp_dirs')
            )
        try:
            os.makedirs(os.path.join(self.dir_path, 'tmp_dirs'))
        except:
            pass

        self.user = self.get_username()
        self.probackup_path = None
        if 'PGPROBACKUPBIN' in self.test_env:
            if (
                os.path.isfile(self.test_env["PGPROBACKUPBIN"]) and
                os.access(self.test_env["PGPROBACKUPBIN"], os.X_OK)
            ):
                self.probackup_path = self.test_env["PGPROBACKUPBIN"]
            else:
                if self.verbose:
                    print('PGPROBACKUPBIN is not an executable file')

        if not self.probackup_path:
            probackup_path_tmp = os.path.join(
                testgres.get_pg_config()['BINDIR'], 'pg_probackup')

            if os.path.isfile(probackup_path_tmp):
                if not os.access(probackup_path_tmp, os.X_OK):
                    print('{0} is not an executable file'.format(
                        probackup_path_tmp))
                else:
                    self.probackup_path = probackup_path_tmp

        if not self.probackup_path:
            probackup_path_tmp = os.path.abspath(os.path.join(
                self.dir_path, '../pg_probackup'))

            if os.path.isfile(probackup_path_tmp):
                if not os.access(probackup_path_tmp, os.X_OK):
                    print('{0} is not an executable file'.format(
                        probackup_path_tmp))
                else:
                    self.probackup_path = probackup_path_tmp

        if not self.probackup_path:
            print('pg_probackup binary is not found')
            exit(1)

        if os.name == 'posix':
            self.EXTERNAL_DIRECTORY_DELIMITER = ':'
            os.environ['PATH'] = os.path.dirname(
                self.probackup_path) + ':' + os.environ['PATH']

        elif os.name == 'nt':
            self.EXTERNAL_DIRECTORY_DELIMITER = ';'
            os.environ['PATH'] = os.path.dirname(
                self.probackup_path) + ';' + os.environ['PATH']

        self.probackup_old_path = None

        if 'PGPROBACKUPBIN_OLD' in self.test_env:
            if (
                os.path.isfile(self.test_env['PGPROBACKUPBIN_OLD']) and
                os.access(self.test_env['PGPROBACKUPBIN_OLD'], os.X_OK)
            ):
                self.probackup_old_path = self.test_env['PGPROBACKUPBIN_OLD']
            else:
                if self.verbose:
                    print('PGPROBACKUPBIN_OLD is not an executable file')

        self.probackup_version = None
        self.old_probackup_version = None

        try:
            self.probackup_version_output = subprocess.check_output(
                [self.probackup_path, "--version"],
                stderr=subprocess.STDOUT,
                ).decode('utf-8')
        except subprocess.CalledProcessError as e:
            raise ProbackupException(e.output.decode('utf-8'))

        if self.probackup_old_path:
            old_probackup_version_output = subprocess.check_output(
                [self.probackup_old_path, "--version"],
                stderr=subprocess.STDOUT,
                ).decode('utf-8')
            self.old_probackup_version = re.search(
                r"\d+\.\d+\.\d+",
                subprocess.check_output(
                    [self.probackup_old_path, "--version"],
                    stderr=subprocess.STDOUT,
                    ).decode('utf-8')
                ).group(0)

        self.probackup_version = re.search(r"\d+\.\d+\.\d+", self.probackup_version_output).group(0)

        self.remote = False
        self.remote_host = None
        self.remote_port = None
        self.remote_user = None

        if 'PGPROBACKUP_SSH_REMOTE' in self.test_env:
            if self.test_env['PGPROBACKUP_SSH_REMOTE'] == 'ON':
                self.remote = True

        self.ptrack = False
        if 'PG_PROBACKUP_PTRACK' in self.test_env:
            if self.test_env['PG_PROBACKUP_PTRACK'] == 'ON':
                self.ptrack = True

        os.environ["PGAPPNAME"] = "pg_probackup"

        if self.ptrack:
            self.assertGreaterEqual(
                self.pg_config_version,
                self.version_to_num('11.0'),
                "ptrack testing require PostgreSQL >= 11")

    @property
    def pg_config_version(self):
        return self.version_to_num(
            testgres.get_pg_config()['VERSION'].split(" ")[1])

#            if 'PGPROBACKUP_SSH_HOST' in self.test_env:
#                self.remote_host = self.test_env['PGPROBACKUP_SSH_HOST']
#            else
#                print('PGPROBACKUP_SSH_HOST is not set')
#                exit(1)
#
#            if 'PGPROBACKUP_SSH_PORT' in self.test_env:
#                self.remote_port = self.test_env['PGPROBACKUP_SSH_PORT']
#            else
#                print('PGPROBACKUP_SSH_PORT is not set')
#                exit(1)
#
#            if 'PGPROBACKUP_SSH_USER' in self.test_env:
#                self.remote_user = self.test_env['PGPROBACKUP_SSH_USER']
#            else
#                print('PGPROBACKUP_SSH_USER is not set')
#                exit(1)

    def make_simple_node(
            self,
            base_dir=None,
            set_replication=False,
            ptrack_enable=False,
            initdb_params=[],
            pg_options={}):

        real_base_dir = os.path.join(self.tmp_path, base_dir)
        shutil.rmtree(real_base_dir, ignore_errors=True)
        os.makedirs(real_base_dir)

        node = testgres.get_new_node('test', base_dir=real_base_dir)
        # bound method slow_start() to 'node' class instance
        node.slow_start = slow_start.__get__(node)
        node.should_rm_dirs = True
        node.init(
           initdb_params=initdb_params, allow_streaming=set_replication)

        # set major version
        with open(os.path.join(node.data_dir, 'PG_VERSION')) as f:
            node.major_version_str = str(f.read().rstrip())
            node.major_version = float(node.major_version_str)

        # Sane default parameters
        options = {}
        options['max_connections'] = 100
        options['shared_buffers'] = '10MB'
        options['fsync'] = 'off'

        options['wal_level'] = 'logical'
        options['hot_standby'] = 'off'

        options['log_line_prefix'] = '%t [%p]: [%l-1] '
        options['log_statement'] = 'none'
        options['log_duration'] = 'on'
        options['log_min_duration_statement'] = 0
        options['log_connections'] = 'on'
        options['log_disconnections'] = 'on'
        options['restart_after_crash'] = 'off'
        options['autovacuum'] = 'off'

        # Allow replication in pg_hba.conf
        if set_replication:
            options['max_wal_senders'] = 10

        if ptrack_enable:
            options['ptrack.map_size'] = '128'
            options['shared_preload_libraries'] = 'ptrack'

        if node.major_version >= 13:
            options['wal_keep_size'] = '200MB'
        else:
            options['wal_keep_segments'] = '100'

        # set default values
        self.set_auto_conf(node, options)

        # Apply given parameters
        self.set_auto_conf(node, pg_options)

        # kludge for testgres
        # https://github.com/postgrespro/testgres/issues/54
        # for PG >= 13 remove 'wal_keep_segments' parameter
        if node.major_version >= 13:
            self.set_auto_conf(
                node, {}, 'postgresql.conf', ['wal_keep_segments'])
        return node
    
    def simple_bootstrap(self, node, role) -> None:

        node.safe_psql(
            'postgres',
            'CREATE ROLE {0} WITH LOGIN REPLICATION'.format(role))

        # PG 9.5
        if self.get_version(node) < 90600:
            node.safe_psql(
                'postgres',
                'GRANT USAGE ON SCHEMA pg_catalog TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_create_restore_point(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_switch_xlog() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO {0};'.format(role))
        # PG 9.6
        elif self.get_version(node) > 90600 and self.get_version(node) < 100000:
            node.safe_psql(
                'postgres',
                'GRANT USAGE ON SCHEMA pg_catalog TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean, boolean) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup(boolean) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_create_restore_point(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_switch_xlog() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_last_xlog_replay_location() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_control_checkpoint() TO {0};'.format(role))
        # >= 10
        else:
            node.safe_psql(
                'postgres',
                'GRANT USAGE ON SCHEMA pg_catalog TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean, boolean) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup(boolean, boolean) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_create_restore_point(text) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_switch_wal() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_last_wal_replay_lsn() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO {0}; '
                'GRANT EXECUTE ON FUNCTION pg_catalog.pg_control_checkpoint() TO {0};'.format(role))

    def create_tblspace_in_node(self, node, tblspc_name, tblspc_path=None, cfs=False):
        res = node.execute(
            'postgres',
            'select exists'
            " (select 1 from pg_tablespace where spcname = '{0}')".format(
                tblspc_name)
            )
        # Check that tablespace with name 'tblspc_name' do not exists already
        self.assertFalse(
            res[0][0],
            'Tablespace "{0}" already exists'.format(tblspc_name)
            )

        if not tblspc_path:
            tblspc_path = os.path.join(
                node.base_dir, '{0}'.format(tblspc_name))
        cmd = "CREATE TABLESPACE {0} LOCATION '{1}'".format(
            tblspc_name, tblspc_path)
        if cfs:
            cmd += ' with (compression=true)'

        if not os.path.exists(tblspc_path):
            os.makedirs(tblspc_path)
        res = node.safe_psql('postgres', cmd)
        # Check that tablespace was successfully created
        # self.assertEqual(
        #     res[0], 0,
        #     'Failed to create tablespace with cmd: {0}'.format(cmd))

    def drop_tblspace(self, node, tblspc_name):
        res = node.execute(
            'postgres',
            'select exists'
            " (select 1 from pg_tablespace where spcname = '{0}')".format(
                tblspc_name)
            )
        # Check that tablespace with name 'tblspc_name' do not exists already
        self.assertTrue(
            res[0][0],
            'Tablespace "{0}" do not exists'.format(tblspc_name)
            )

        rels = node.execute(
            "postgres",
            "SELECT relname FROM pg_class c "
            "LEFT JOIN pg_tablespace t ON c.reltablespace = t.oid "
            "where c.relkind = 'r' and t.spcname = '{0}'".format(tblspc_name))

        for rel in rels:
            node.safe_psql(
                'postgres',
                "DROP TABLE {0}".format(rel[0]))

        node.safe_psql(
            'postgres',
            'DROP TABLESPACE {0}'.format(tblspc_name))


    def get_tblspace_path(self, node, tblspc_name):
        return os.path.join(node.base_dir, tblspc_name)

    def get_fork_size(self, node, fork_name):
        return node.execute(
            'postgres',
            "select pg_relation_size('{0}')/8192".format(fork_name))[0][0]

    def get_fork_path(self, node, fork_name):
        return os.path.join(
            node.base_dir, 'data', node.execute(
                'postgres',
                "select pg_relation_filepath('{0}')".format(
                    fork_name))[0][0]
            )

    def get_md5_per_page_for_fork(self, file, size_in_pages):
        pages_per_segment = {}
        md5_per_page = {}
        size_in_pages = int(size_in_pages)
        nsegments = int(size_in_pages/131072)
        if size_in_pages % 131072 != 0:
            nsegments = nsegments + 1

        size = size_in_pages
        for segment_number in range(nsegments):
            if size - 131072 > 0:
                pages_per_segment[segment_number] = 131072
            else:
                pages_per_segment[segment_number] = size
            size = size - 131072

        for segment_number in range(nsegments):
            offset = 0
            if segment_number == 0:
                file_desc = os.open(file, os.O_RDONLY)
                start_page = 0
                end_page = pages_per_segment[segment_number]
            else:
                file_desc = os.open(
                    file+'.{0}'.format(segment_number), os.O_RDONLY
                    )
                start_page = max(md5_per_page)+1
                end_page = end_page + pages_per_segment[segment_number]

            for page in range(start_page, end_page):
                md5_per_page[page] = hashlib.md5(
                    os.read(file_desc, 8192)).hexdigest()
                offset += 8192
                os.lseek(file_desc, offset, 0)
            os.close(file_desc)

        return md5_per_page

    def get_ptrack_bits_per_page_for_fork(self, node, file, size=[]):

        if self.get_pgpro_edition(node) == 'enterprise':
            if self.get_version(node) < self.version_to_num('10.0'):
                header_size = 48
            else:
                header_size = 24
        else:
            header_size = 24
        ptrack_bits_for_fork = []

        # TODO: use macro instead of hard coded 8KB
        page_body_size = 8192-header_size
        # Check that if main fork file size is 0, it`s ok
        # to not having a _ptrack fork
        if os.path.getsize(file) == 0:
            return ptrack_bits_for_fork
        byte_size = os.path.getsize(file + '_ptrack')
        npages = int(byte_size/8192)
        if byte_size % 8192 != 0:
            print('Ptrack page is not 8k aligned')
            exit(1)

        file = os.open(file + '_ptrack', os.O_RDONLY)

        for page in range(npages):
            offset = 8192*page+header_size
            os.lseek(file, offset, 0)
            lots_of_bytes = os.read(file, page_body_size)
            byte_list = [
                lots_of_bytes[i:i+1] for i in range(len(lots_of_bytes))
                ]
            for byte in byte_list:
                # byte_inverted = bin(int(byte, base=16))[2:][::-1]
                # bits = (byte >> x) & 1 for x in range(7, -1, -1)
                byte_inverted = bin(ord(byte))[2:].rjust(8, '0')[::-1]
                for bit in byte_inverted:
                    # if len(ptrack_bits_for_fork) < size:
                    ptrack_bits_for_fork.append(int(bit))

        os.close(file)
        return ptrack_bits_for_fork

    def check_ptrack_map_sanity(self, node, idx_ptrack):
        success = True
        for i in idx_ptrack:
            # get new size of heap and indexes. size calculated in pages
            idx_ptrack[i]['new_size'] = self.get_fork_size(node, i)
            # update path to heap and index files in case they`ve changed
            idx_ptrack[i]['path'] = self.get_fork_path(node, i)
            # calculate new md5sums for pages
            idx_ptrack[i]['new_pages'] = self.get_md5_per_page_for_fork(
                idx_ptrack[i]['path'], idx_ptrack[i]['new_size'])
            # get ptrack for every idx
            idx_ptrack[i]['ptrack'] = self.get_ptrack_bits_per_page_for_fork(
                node, idx_ptrack[i]['path'],
                [idx_ptrack[i]['old_size'], idx_ptrack[i]['new_size']])

            # compare pages and check ptrack sanity
            if not self.check_ptrack_sanity(idx_ptrack[i]):
                success = False

        self.assertTrue(
            success, 'Ptrack has failed to register changes in data files')

    def check_ptrack_sanity(self, idx_dict):
        success = True
        if idx_dict['new_size'] > idx_dict['old_size']:
            size = idx_dict['new_size']
        else:
            size = idx_dict['old_size']
        for PageNum in range(size):
            if PageNum not in idx_dict['old_pages']:
                # Page was not present before, meaning that relation got bigger
                # Ptrack should be equal to 1
                if idx_dict['ptrack'][PageNum] != 1:
                    if self.verbose:
                        print(
                            'File: {0}\n Page Number {1} of type {2} was added,'
                            ' but ptrack value is {3}. THIS IS BAD'.format(
                                idx_dict['path'],
                                PageNum, idx_dict['type'],
                                idx_dict['ptrack'][PageNum])
                        )
                        # print(idx_dict)
                    success = False
                continue
            if PageNum not in idx_dict['new_pages']:
                # Page is not present now, meaning that relation got smaller
                # Ptrack should be equal to 1,
                # We are not freaking out about false positive stuff
                if idx_dict['ptrack'][PageNum] != 1:
                    if self.verbose:
                        print(
                            'File: {0}\n Page Number {1} of type {2} was deleted,'
                            ' but ptrack value is {3}. THIS IS BAD'.format(
                                idx_dict['path'],
                                PageNum, idx_dict['type'],
                                idx_dict['ptrack'][PageNum])
                        )
                continue

            # Ok, all pages in new_pages that do not have
            # corresponding page in old_pages are been dealt with.
            # We can now safely proceed to comparing old and new pages
            if idx_dict['new_pages'][
                    PageNum] != idx_dict['old_pages'][PageNum]:
                # Page has been changed,
                # meaning that ptrack should be equal to 1
                if idx_dict['ptrack'][PageNum] != 1:
                    if self.verbose:
                        print(
                            'File: {0}\n Page Number {1} of type {2} was changed,'
                            ' but ptrack value is {3}. THIS IS BAD'.format(
                                idx_dict['path'],
                                PageNum, idx_dict['type'],
                                idx_dict['ptrack'][PageNum])
                        )
                        print(
                            '  Old checksumm: {0}\n'
                            '  New checksumm: {1}'.format(
                                idx_dict['old_pages'][PageNum],
                                idx_dict['new_pages'][PageNum])
                        )

                    if PageNum == 0 and idx_dict['type'] == 'spgist':
                        if self.verbose:
                            print(
                                'SPGIST is a special snowflake, so don`t '
                                'fret about losing ptrack for blknum 0'
                            )
                        continue
                    success = False
            else:
                # Page has not been changed,
                # meaning that ptrack should be equal to 0
                if idx_dict['ptrack'][PageNum] != 0:
                    if self.verbose:
                        print(
                            'File: {0}\n Page Number {1} of type {2} was not changed,'
                            ' but ptrack value is {3}'.format(
                                idx_dict['path'],
                                PageNum, idx_dict['type'],
                                idx_dict['ptrack'][PageNum]
                            )
                        )
            return success
            # self.assertTrue(
            #    success, 'Ptrack has failed to register changes in data files'
            # )

    def get_backup_filelist(self, backup_dir, instance, backup_id):

        filelist_path = os.path.join(
            backup_dir, 'backups',
            instance, backup_id, 'backup_content.control')

        with open(filelist_path, 'r') as f:
                filelist_raw = f.read()

        filelist_splitted = filelist_raw.splitlines()

        filelist = {}
        for line in filelist_splitted:
            line = json.loads(line)
            filelist[line['path']] = line

        return filelist

    # return dict of files from filelist A,
    # which are not exists in filelist_B
    def get_backup_filelist_diff(self, filelist_A, filelist_B):

        filelist_diff = {}
        for file in filelist_A:
            if file not in filelist_B:
                filelist_diff[file] = filelist_A[file]

        return filelist_diff

    # used for partial restore
    def truncate_every_file_in_dir(self, path):
        for file in os.listdir(path):
            with open(os.path.join(path, file), "w") as f:
                f.close()

    def check_ptrack_recovery(self, idx_dict):
        size = idx_dict['size']
        for PageNum in range(size):
            if idx_dict['ptrack'][PageNum] != 1:
                self.assertTrue(
                    False,
                    'Recovery for Page Number {0} of Type {1}'
                    ' was conducted, but ptrack value is {2}.'
                    ' THIS IS BAD\n IDX_DICT: {3}'.format(
                        PageNum, idx_dict['type'],
                        idx_dict['ptrack'][PageNum],
                        idx_dict
                    )
                )

    def check_ptrack_clean(self, idx_dict, size):
        for PageNum in range(size):
            if idx_dict['ptrack'][PageNum] != 0:
                self.assertTrue(
                    False,
                    'Ptrack for Page Number {0} of Type {1}'
                    ' should be clean, but ptrack value is {2}.'
                    '\n THIS IS BAD\n IDX_DICT: {3}'.format(
                        PageNum,
                        idx_dict['type'],
                        idx_dict['ptrack'][PageNum],
                        idx_dict
                    )
                )

    def run_pb(self, command, asynchronous=False, gdb=False, old_binary=False, return_id=True, env=None):
        if not self.probackup_old_path and old_binary:
            print('PGPROBACKUPBIN_OLD is not set')
            exit(1)

        if old_binary:
            binary_path = self.probackup_old_path
        else:
            binary_path = self.probackup_path

        if not env:
            env=self.test_env

        try:
            self.cmd = [' '.join(map(str, [binary_path] + command))]
            if self.verbose:
                print(self.cmd)
            if gdb:
                return GDBobj([binary_path] + command, self.verbose)
            if asynchronous:
                return subprocess.Popen(
                    [binary_path] + command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=env
                )
            else:
                self.output = subprocess.check_output(
                    [binary_path] + command,
                    stderr=subprocess.STDOUT,
                    env=env
                    ).decode('utf-8')
                if command[0] == 'backup' and return_id:
                    # return backup ID
                    for line in self.output.splitlines():
                        if 'INFO: Backup' and 'completed' in line:
                            return line.split()[2]
                else:
                    return self.output
        except subprocess.CalledProcessError as e:
            raise ProbackupException(e.output.decode('utf-8'), self.cmd)

    def run_binary(self, command, asynchronous=False, env=None):

        if not env:
            env = self.test_env

        if self.verbose:
                print([' '.join(map(str, command))])
        try:
            if asynchronous:
                return subprocess.Popen(
                    command,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=env
                )
            else:
                self.output = subprocess.check_output(
                    command,
                    stderr=subprocess.STDOUT,
                    env=env
                    ).decode('utf-8')
                return self.output
        except subprocess.CalledProcessError as e:
            raise ProbackupException(e.output.decode('utf-8'), command)

    def init_pb(self, backup_dir, options=[], old_binary=False):

        shutil.rmtree(backup_dir, ignore_errors=True)

        # don`t forget to kill old_binary after remote ssh release
        if self.remote and not old_binary:
            options = options + [
                '--remote-proto=ssh',
                '--remote-host=localhost']

        return self.run_pb([
            'init',
            '-B', backup_dir
            ] + options,
            old_binary=old_binary
        )

    def add_instance(self, backup_dir, instance, node, old_binary=False, options=[]):

        cmd = [
            'add-instance',
            '--instance={0}'.format(instance),
            '-B', backup_dir,
            '-D', node.data_dir
            ]

        # don`t forget to kill old_binary after remote ssh release
        if self.remote and not old_binary:
            options = options + [
                '--remote-proto=ssh',
                '--remote-host=localhost']

        return self.run_pb(cmd + options, old_binary=old_binary)

    def set_config(self, backup_dir, instance, old_binary=False, options=[]):

        cmd = [
            'set-config',
            '--instance={0}'.format(instance),
            '-B', backup_dir,
            ]

        return self.run_pb(cmd + options, old_binary=old_binary)

    def set_backup(self, backup_dir, instance, backup_id=False,
                    old_binary=False, options=[]):

        cmd = [
            'set-backup',
            '-B', backup_dir
            ]

        if instance:
            cmd = cmd + ['--instance={0}'.format(instance)]

        if backup_id:
            cmd = cmd + ['-i', backup_id]

        return self.run_pb(cmd + options, old_binary=old_binary)

    def del_instance(self, backup_dir, instance, old_binary=False):

        return self.run_pb([
            'del-instance',
            '--instance={0}'.format(instance),
            '-B', backup_dir
            ],
            old_binary=old_binary
        )

    def clean_pb(self, backup_dir):
        shutil.rmtree(backup_dir, ignore_errors=True)

    def backup_node(
            self, backup_dir, instance, node, data_dir=False,
            backup_type='full', datname=False, options=[],
            asynchronous=False, gdb=False,
            old_binary=False, return_id=True, no_remote=False,
            env=None
            ):
        if not node and not data_dir:
            print('You must provide ether node or data_dir for backup')
            exit(1)

        if not datname:
            datname = 'postgres'

        cmd_list = [
            'backup',
            '-B', backup_dir,
            '--instance={0}'.format(instance),
            # "-D", pgdata,
            '-p', '%i' % node.port,
            '-d', datname
        ]

        if data_dir:
            cmd_list += ['-D', data_dir]

        # don`t forget to kill old_binary after remote ssh release
        if self.remote and not old_binary and not no_remote:
            options = options + [
                '--remote-proto=ssh',
                '--remote-host=localhost']

        if backup_type:
            cmd_list += ['-b', backup_type]

        if not old_binary:
            cmd_list += ['--no-sync']

        return self.run_pb(cmd_list + options, asynchronous, gdb, old_binary, return_id, env=env)

    def checkdb_node(
            self, backup_dir=False, instance=False, data_dir=False,
            options=[], asynchronous=False, gdb=False, old_binary=False
            ):

        cmd_list = ["checkdb"]

        if backup_dir:
            cmd_list += ["-B", backup_dir]

        if instance:
            cmd_list += ["--instance={0}".format(instance)]

        if data_dir:
            cmd_list += ["-D", data_dir]

        return self.run_pb(cmd_list + options, asynchronous, gdb, old_binary)

    def merge_backup(
            self, backup_dir, instance, backup_id, asynchronous=False,
            gdb=False, old_binary=False, options=[]):
        cmd_list = [
            'merge',
            '-B', backup_dir,
            '--instance={0}'.format(instance),
            '-i', backup_id
        ]

        return self.run_pb(cmd_list + options, asynchronous, gdb, old_binary)

    def restore_node(
            self, backup_dir, instance, node=False,
            data_dir=None, backup_id=None, old_binary=False, options=[],
            gdb=False
            ):

        if data_dir is None:
            data_dir = node.data_dir

        cmd_list = [
            'restore',
            '-B', backup_dir,
            '-D', data_dir,
            '--instance={0}'.format(instance)
        ]

        # don`t forget to kill old_binary after remote ssh release
        if self.remote and not old_binary:
            options = options + [
                '--remote-proto=ssh',
                '--remote-host=localhost']

        if backup_id:
            cmd_list += ['-i', backup_id]

        if not old_binary:
            cmd_list += ['--no-sync']

        return self.run_pb(cmd_list + options, gdb=gdb, old_binary=old_binary)

    def show_pb(
            self, backup_dir, instance=None, backup_id=None,
            options=[], as_text=False, as_json=True, old_binary=False,
            env=None
            ):

        backup_list = []
        specific_record = {}
        cmd_list = [
            'show',
            '-B', backup_dir,
        ]
        if instance:
            cmd_list += ['--instance={0}'.format(instance)]

        if backup_id:
            cmd_list += ['-i', backup_id]

        # AHTUNG, WARNING will break json parsing
        if as_json:
            cmd_list += ['--format=json', '--log-level-console=error']

        if as_text:
            # You should print it when calling as_text=true
            return self.run_pb(cmd_list + options, old_binary=old_binary, env=env)

        # get show result as list of lines
        if as_json:
            data = json.loads(self.run_pb(cmd_list + options, old_binary=old_binary))
        #    print(data)
            for instance_data in data:
                # find specific instance if requested
                if instance and instance_data['instance'] != instance:
                    continue

                for backup in reversed(instance_data['backups']):
                    # find specific backup if requested
                    if backup_id:
                        if backup['id'] == backup_id:
                            return backup
                    else:
                        backup_list.append(backup)

            if backup_id is not None:
                self.assertTrue(False, "Failed to find backup with ID: {0}".format(backup_id))

            return backup_list
        else:
            show_splitted = self.run_pb(
                cmd_list + options, old_binary=old_binary, env=env).splitlines()
            if instance is not None and backup_id is None:
                # cut header(ID, Mode, etc) from show as single string
                header = show_splitted[1:2][0]
                # cut backup records from show as single list
                # with string for every backup record
                body = show_splitted[3:]
                # inverse list so oldest record come first
                body = body[::-1]
                # split string in list with string for every header element
                header_split = re.split('  +', header)
                # Remove empty items
                for i in header_split:
                    if i == '':
                        header_split.remove(i)
                        continue
                header_split = [
                    header_element.rstrip() for header_element in header_split
                    ]
                for backup_record in body:
                    backup_record = backup_record.rstrip()
                    # split list with str for every backup record element
                    backup_record_split = re.split('  +', backup_record)
                    # Remove empty items
                    for i in backup_record_split:
                        if i == '':
                            backup_record_split.remove(i)
                    if len(header_split) != len(backup_record_split):
                        print(warning.format(
                            header=header, body=body,
                            header_split=header_split,
                            body_split=backup_record_split)
                        )
                        exit(1)
                    new_dict = dict(zip(header_split, backup_record_split))
                    backup_list.append(new_dict)
                return backup_list
            else:
                # cut out empty lines and lines started with #
                # and other garbage then reconstruct it as dictionary
                # print show_splitted
                sanitized_show = [item for item in show_splitted if item]
                sanitized_show = [
                    item for item in sanitized_show if not item.startswith('#')
                ]
                # print sanitized_show
                for line in sanitized_show:
                    name, var = line.partition(' = ')[::2]
                    var = var.strip('"')
                    var = var.strip("'")
                    specific_record[name.strip()] = var

                if not specific_record:
                    self.assertTrue(False, "Failed to find backup with ID: {0}".format(backup_id))

                return specific_record

    def show_archive(
            self, backup_dir, instance=None, options=[],
            as_text=False, as_json=True, old_binary=False,
            tli=0
            ):

        cmd_list = [
            'show',
            '--archive',
            '-B', backup_dir,
        ]
        if instance:
            cmd_list += ['--instance={0}'.format(instance)]

        # AHTUNG, WARNING will break json parsing
        if as_json:
            cmd_list += ['--format=json', '--log-level-console=error']

        if as_text:
            # You should print it when calling as_text=true
            return self.run_pb(cmd_list + options, old_binary=old_binary)

        if as_json:
            if as_text:
                data = self.run_pb(cmd_list + options, old_binary=old_binary)
            else:
                data = json.loads(self.run_pb(cmd_list + options, old_binary=old_binary))

            if instance:
                instance_timelines = None
                for instance_name in data:
                    if instance_name['instance'] == instance:
                        instance_timelines = instance_name['timelines']
                        break

                if tli > 0:
                    timeline_data = None
                    for timeline in instance_timelines:
                        if timeline['tli'] == tli:
                            return timeline

                    return {}

                if instance_timelines:
                    return instance_timelines

            return data
        else:
            show_splitted = self.run_pb(
                cmd_list + options, old_binary=old_binary).splitlines()
            print(show_splitted)
            exit(1)

    def validate_pb(
            self, backup_dir, instance=None, backup_id=None,
            options=[], old_binary=False, gdb=False, asynchronous=False
            ):

        cmd_list = [
            'validate',
            '-B', backup_dir
        ]
        if instance:
            cmd_list += ['--instance={0}'.format(instance)]
        if backup_id:
            cmd_list += ['-i', backup_id]

        return self.run_pb(cmd_list + options, old_binary=old_binary, gdb=gdb, asynchronous=asynchronous)

    def delete_pb(
            self, backup_dir, instance, backup_id=None,
            options=[], old_binary=False, gdb=False, asynchronous=False):
        cmd_list = [
            'delete',
            '-B', backup_dir
        ]

        cmd_list += ['--instance={0}'.format(instance)]
        if backup_id:
            cmd_list += ['-i', backup_id]

        return self.run_pb(cmd_list + options, old_binary=old_binary, gdb=gdb, asynchronous=asynchronous)

    def delete_expired(
            self, backup_dir, instance, options=[], old_binary=False):
        cmd_list = [
            'delete',
            '-B', backup_dir,
            '--instance={0}'.format(instance)
        ]
        return self.run_pb(cmd_list + options, old_binary=old_binary)

    def show_config(self, backup_dir, instance, old_binary=False):
        out_dict = {}
        cmd_list = [
            'show-config',
            '-B', backup_dir,
            '--instance={0}'.format(instance)
        ]

        res = self.run_pb(cmd_list, old_binary=old_binary).splitlines()
        for line in res:
            if not line.startswith('#'):
                name, var = line.partition(' = ')[::2]
                out_dict[name] = var
        return out_dict

    def get_recovery_conf(self, node):
        out_dict = {}

        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf_path = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf_path, 'r') as f:
                print(f.read())
        else:
            recovery_conf_path = os.path.join(node.data_dir, 'recovery.conf')

        with open(
            recovery_conf_path, 'r'
        ) as recovery_conf:
            for line in recovery_conf:
                try:
                    key, value = line.split('=')
                except:
                    continue
                out_dict[key.strip()] = value.strip(" '").replace("'\n", "")
        return out_dict

    def set_archiving(
            self, backup_dir, instance, node, replica=False,
            overwrite=False, compress=True, old_binary=False,
            log_level=False, archive_timeout=False):

        # parse postgresql.auto.conf
        options = {}
        if replica:
            options['archive_mode'] = 'always'
            options['hot_standby'] = 'on'
        else:
            options['archive_mode'] = 'on'

        if os.name == 'posix':
            options['archive_command'] = '"{0}" archive-push -B {1} --instance={2} '.format(
                self.probackup_path, backup_dir, instance)

        elif os.name == 'nt':
            options['archive_command'] = '"{0}" archive-push -B {1} --instance={2} '.format(
                self.probackup_path.replace("\\","\\\\"),
                backup_dir.replace("\\","\\\\"), instance)

        # don`t forget to kill old_binary after remote ssh release
        if self.remote and not old_binary:
            options['archive_command'] += '--remote-proto=ssh '
            options['archive_command'] += '--remote-host=localhost '

        if self.archive_compress and compress:
            options['archive_command'] += '--compress '

        if overwrite:
            options['archive_command'] += '--overwrite '

        options['archive_command'] += '--log-level-console=VERBOSE '
        options['archive_command'] += '-j 5 '
        options['archive_command'] += '--batch-size 10 '
        options['archive_command'] += '--no-sync '

        if archive_timeout:
            options['archive_command'] += '--archive-timeout={0} '.format(
                archive_timeout)

        if os.name == 'posix':
            options['archive_command'] += '--wal-file-path=%p --wal-file-name=%f'

        elif os.name == 'nt':
            options['archive_command'] += '--wal-file-path="%p" --wal-file-name="%f"'

        if log_level:
            options['archive_command'] += ' --log-level-console={0}'.format(log_level)
            options['archive_command'] += ' --log-level-file={0} '.format(log_level)


        self.set_auto_conf(node, options)

    def get_restore_command(self, backup_dir, instance, node):

        # parse postgresql.auto.conf
        restore_command = ''
        if os.name == 'posix':
            restore_command += '{0} archive-get -B {1} --instance={2} '.format(
                self.probackup_path, backup_dir, instance)

        elif os.name == 'nt':
            restore_command += '"{0}" archive-get -B {1} --instance={2} '.format(
                self.probackup_path.replace("\\","\\\\"),
                backup_dir.replace("\\","\\\\"), instance)

        # don`t forget to kill old_binary after remote ssh release
        if self.remote:
            restore_command += '--remote-proto=ssh '
            restore_command += '--remote-host=localhost '

        if os.name == 'posix':
            restore_command += '--wal-file-path=%p --wal-file-name=%f'

        elif os.name == 'nt':
            restore_command += '--wal-file-path="%p" --wal-file-name="%f"'

        return restore_command

    # rm_options - list of parameter name that should be deleted from current config,
    # example: ['wal_keep_segments', 'max_wal_size']
    def set_auto_conf(self, node, options, config='postgresql.auto.conf', rm_options={}):

        # parse postgresql.auto.conf
        path = os.path.join(node.data_dir, config)

        with open(path, 'r') as f:
            raw_content = f.read()

        current_options = {}
        current_directives = []
        for line in raw_content.splitlines():

            # ignore comments
            if line.startswith('#'):
                continue

            if line == '':
                continue

            if line.startswith('include'):
                current_directives.append(line)
                continue

            name, var = line.partition('=')[::2]
            name = name.strip()
            var = var.strip()
            var = var.strip('"')
            var = var.strip("'")

            # remove options specified in rm_options list
            if name in rm_options:
                continue

            current_options[name] = var

        for option in options:
            current_options[option] = options[option]

        auto_conf = ''
        for option in current_options:
            auto_conf += "{0} = '{1}'\n".format(
                option, current_options[option])

        for directive in current_directives:
            auto_conf += directive + "\n"

        with open(path, 'wt') as f:
            f.write(auto_conf)
            f.flush()
            f.close()

    def set_replica(
            self, master, replica,
            replica_name='replica',
            synchronous=False,
            log_shipping=False
            ):

        self.set_auto_conf(
            replica,
            options={
                'port': replica.port,
                'hot_standby': 'on'})

        if self.get_version(replica) >= self.version_to_num('12.0'):
            with open(os.path.join(replica.data_dir, "standby.signal"), 'w') as f:
                f.flush()
                f.close()

            config = 'postgresql.auto.conf'

            if not log_shipping:
                self.set_auto_conf(
                    replica,
                    {'primary_conninfo': 'user={0} port={1} application_name={2} '
                    ' sslmode=prefer sslcompression=1'.format(
                        self.user, master.port, replica_name)},
                    config)
        else:
            replica.append_conf('recovery.conf', 'standby_mode = on')

            if not log_shipping:
                replica.append_conf(
                    'recovery.conf',
                    "primary_conninfo = 'user={0} port={1} application_name={2}"
                    " sslmode=prefer sslcompression=1'".format(
                        self.user, master.port, replica_name))

        if synchronous:
            self.set_auto_conf(
                master,
                options={
                    'synchronous_standby_names': replica_name,
                    'synchronous_commit': 'remote_apply'})

            master.reload()

    def change_backup_status(self, backup_dir, instance, backup_id, status):

        control_file_path = os.path.join(
            backup_dir, 'backups', instance, backup_id, 'backup.control')

        with open(control_file_path, 'r') as f:
            actual_control = f.read()

        new_control_file = ''
        for line in actual_control.splitlines():
            if line.startswith('status'):
                line = 'status = {0}'.format(status)
            new_control_file += line
            new_control_file += '\n'

        with open(control_file_path, 'wt') as f:
            f.write(new_control_file)
            f.flush()
            f.close()

        with open(control_file_path, 'r') as f:
            actual_control = f.read()

    def wrong_wal_clean(self, node, wal_size):
        wals_dir = os.path.join(self.backup_dir(node), 'wal')
        wals = [
            f for f in os.listdir(wals_dir) if os.path.isfile(
                os.path.join(wals_dir, f))
        ]
        wals.sort()
        file_path = os.path.join(wals_dir, wals[-1])
        if os.path.getsize(file_path) != wal_size:
            os.remove(file_path)

    def guc_wal_segment_size(self, node):
        var = node.execute(
            'postgres',
            "select setting from pg_settings where name = 'wal_segment_size'"
        )
        return int(var[0][0]) * self.guc_wal_block_size(node)

    def guc_wal_block_size(self, node):
        var = node.execute(
            'postgres',
            "select setting from pg_settings where name = 'wal_block_size'"
        )
        return int(var[0][0])

    def get_pgpro_edition(self, node):
        if node.execute(
            'postgres',
            "select exists (select 1 from"
            " pg_proc where proname = 'pgpro_edition')"
        )[0][0]:
            var = node.execute('postgres', 'select pgpro_edition()')
            return str(var[0][0])
        else:
            return False

    def get_username(self):
        """ Returns current user name """
        return getpass.getuser()

    def version_to_num(self, version):
        if not version:
            return 0
        parts = version.split('.')
        while len(parts) < 3:
            parts.append('0')
        num = 0
        for part in parts:
            num = num * 100 + int(re.sub("[^\d]", "", part))
        return num

    def switch_wal_segment(self, node):
        """
        Execute pg_switch_wal/xlog() in given node

        Args:
            node: an instance of PostgresNode or NodeConnection class
        """
        if isinstance(node, testgres.PostgresNode):
            if self.version_to_num(
                node.safe_psql('postgres', 'show server_version').decode('utf-8')
                    ) >= self.version_to_num('10.0'):
                node.safe_psql('postgres', 'select pg_switch_wal()')
            else:
                node.safe_psql('postgres', 'select pg_switch_xlog()')
        else:
            if self.version_to_num(
                node.execute('show server_version')[0][0]
                    ) >= self.version_to_num('10.0'):
                node.execute('select pg_switch_wal()')
            else:
                node.execute('select pg_switch_xlog()')

        sleep(1)

    def wait_until_replica_catch_with_master(self, master, replica):

        version = master.safe_psql(
            'postgres',
            'show server_version').decode('utf-8').rstrip()

        if self.version_to_num(version) >= self.version_to_num('10.0'):
            master_function = 'pg_catalog.pg_current_wal_lsn()'
            replica_function = 'pg_catalog.pg_last_wal_replay_lsn()'
        else:
            master_function = 'pg_catalog.pg_current_xlog_location()'
            replica_function = 'pg_catalog.pg_last_xlog_replay_location()'

        lsn = master.safe_psql(
            'postgres',
            'SELECT {0}'.format(master_function)).decode('utf-8').rstrip()

        # Wait until replica catch up with master
        replica.poll_query_until(
            'postgres',
            "SELECT '{0}'::pg_lsn <= {1}".format(lsn, replica_function))

    def get_version(self, node):
        return self.version_to_num(
            testgres.get_pg_config()['VERSION'].split(" ")[1])

    def get_bin_path(self, binary):
        return testgres.get_bin_path(binary)

    def clean_all(self):
        for o in gc.get_referrers(testgres.PostgresNode):
            if o.__class__ is testgres.PostgresNode:
                o.cleanup()

    def del_test_dir(self, module_name, fname):
        """ Del testdir and optimistically try to del module dir"""

        self.clean_all()

        shutil.rmtree(
            os.path.join(
                self.tmp_path,
                module_name,
                fname
            ),
            ignore_errors=True
        )

    def pgdata_content(self, pgdata, ignore_ptrack=True, exclude_dirs=None):
        """ return dict with directory content. "
        " TAKE IT AFTER CHECKPOINT or BACKUP"""
        dirs_to_ignore = [
            'pg_xlog', 'pg_wal', 'pg_log',
            'pg_stat_tmp', 'pg_subtrans', 'pg_notify'
        ]
        files_to_ignore = [
            'postmaster.pid', 'postmaster.opts',
            'pg_internal.init', 'postgresql.auto.conf',
            'backup_label', 'backup_label.old', 'tablespace_map',
            'recovery.conf', 'recovery.done', 'ptrack_control',
            'ptrack_init', 'pg_control', 'probackup_recovery.conf',
            'recovery.signal', 'standby.signal',
            'ptrack.map', 'ptrack.map.mmap', 'ptrack.map.tmp'
        ]

        if exclude_dirs:
            dirs_to_ignore = dirs_to_ignore + exclude_dirs
#        suffixes_to_ignore = (
#            '_ptrack'
#        )
        directory_dict = {}
        directory_dict['pgdata'] = pgdata
        directory_dict['files'] = {}
        directory_dict['dirs'] = {}
        for root, dirs, files in os.walk(pgdata, followlinks=True):
            dirs[:] = [d for d in dirs if d not in dirs_to_ignore]
            for file in files:
                if (
                    file in files_to_ignore or
                    (ignore_ptrack and file.endswith('_ptrack'))
                ):
                        continue

                file_fullpath = os.path.join(root, file)
                file_relpath = os.path.relpath(file_fullpath, pgdata)
                directory_dict['files'][file_relpath] = {'is_datafile': False}
                with open(file_fullpath, 'rb') as f:
                    directory_dict['files'][file_relpath]['md5'] = hashlib.md5(f.read()).hexdigest()
                    f.close()
#                directory_dict['files'][file_relpath]['md5'] = hashlib.md5(
#                    f = open(file_fullpath, 'rb').read()).hexdigest()

                # crappy algorithm
                if file.isdigit():
                    directory_dict['files'][file_relpath]['is_datafile'] = True
                    size_in_pages = os.path.getsize(file_fullpath)/8192
                    directory_dict['files'][file_relpath][
                        'md5_per_page'] = self.get_md5_per_page_for_fork(
                            file_fullpath, size_in_pages
                        )

        for root, dirs, files in os.walk(pgdata, topdown=False, followlinks=True):
            for directory in dirs:
                directory_path = os.path.join(root, directory)
                directory_relpath = os.path.relpath(directory_path, pgdata)

                found = False
                for d in dirs_to_ignore:
                    if d in directory_relpath:
                        found = True
                        break

                # check if directory already here as part of larger directory
                if not found:
                    for d in directory_dict['dirs']:
                        # print("OLD dir {0}".format(d))
                        if directory_relpath in d:
                            found = True
                            break

                if not found:
                    directory_dict['dirs'][directory_relpath] = {}

        # get permissions for every file and directory
        for file in directory_dict['dirs']:
            full_path = os.path.join(pgdata, file)
            directory_dict['dirs'][file]['mode'] = os.stat(
                full_path).st_mode

        for file in directory_dict['files']:
            full_path = os.path.join(pgdata, file)
            directory_dict['files'][file]['mode'] = os.stat(
                full_path).st_mode

        return directory_dict

    def compare_pgdata(self, original_pgdata, restored_pgdata):
        """ return dict with directory content. DO IT BEFORE RECOVERY"""
        fail = False
        error_message = 'Restored PGDATA is not equal to original!\n'

        # Compare directories
        for directory in restored_pgdata['dirs']:
            if directory not in original_pgdata['dirs']:
                fail = True
                error_message += '\nDirectory was not present'
                error_message += ' in original PGDATA: {0}\n'.format(
                    os.path.join(restored_pgdata['pgdata'], directory))
            else:
                if (
                    restored_pgdata['dirs'][directory]['mode'] !=
                    original_pgdata['dirs'][directory]['mode']
                ):
                    fail = True
                    error_message += '\nDir permissions mismatch:\n'
                    error_message += ' Dir old: {0} Permissions: {1}\n'.format(
                        os.path.join(original_pgdata['pgdata'], directory),
                        original_pgdata['dirs'][directory]['mode'])
                    error_message += ' Dir new: {0} Permissions: {1}\n'.format(
                        os.path.join(restored_pgdata['pgdata'], directory),
                        restored_pgdata['dirs'][directory]['mode'])

        for directory in original_pgdata['dirs']:
            if directory not in restored_pgdata['dirs']:
                fail = True
                error_message += '\nDirectory dissappeared'
                error_message += ' in restored PGDATA: {0}\n'.format(
                    os.path.join(restored_pgdata['pgdata'], directory))

        for file in restored_pgdata['files']:
            # File is present in RESTORED PGDATA
            # but not present in ORIGINAL
            # only backup_label is allowed
            if file not in original_pgdata['files']:
                fail = True
                error_message += '\nFile is not present'
                error_message += ' in original PGDATA: {0}\n'.format(
                    os.path.join(restored_pgdata['pgdata'], file))

        for file in original_pgdata['files']:
            if file in restored_pgdata['files']:

                if (
                    restored_pgdata['files'][file]['mode'] !=
                    original_pgdata['files'][file]['mode']
                ):
                    fail = True
                    error_message += '\nFile permissions mismatch:\n'
                    error_message += ' File_old: {0} Permissions: {1}\n'.format(
                        os.path.join(original_pgdata['pgdata'], file),
                        original_pgdata['files'][file]['mode'])
                    error_message += ' File_new: {0} Permissions: {1}\n'.format(
                        os.path.join(restored_pgdata['pgdata'], file),
                        restored_pgdata['files'][file]['mode'])

                if (
                    original_pgdata['files'][file]['md5'] !=
                    restored_pgdata['files'][file]['md5']
                ):
                    fail = True
                    error_message += (
                        '\nFile Checksumm mismatch.\n'
                        'File_old: {0}\nChecksumm_old: {1}\n'
                        'File_new: {2}\nChecksumm_new: {3}\n').format(
                        os.path.join(original_pgdata['pgdata'], file),
                        original_pgdata['files'][file]['md5'],
                        os.path.join(restored_pgdata['pgdata'], file),
                        restored_pgdata['files'][file]['md5']
                    )

                    if original_pgdata['files'][file]['is_datafile']:
                        for page in original_pgdata['files'][file]['md5_per_page']:
                            if page not in restored_pgdata['files'][file]['md5_per_page']:
                                error_message += (
                                    '\n Page {0} dissappeared.\n '
                                    'File: {1}\n').format(
                                        page,
                                        os.path.join(
                                            restored_pgdata['pgdata'],
                                            file
                                        )
                                    )
                                continue

                            if original_pgdata['files'][file][
                                'md5_per_page'][page] != restored_pgdata[
                                    'files'][file]['md5_per_page'][page]:
                                    error_message += (
                                        '\n Page checksumm mismatch: {0}\n '
                                        ' PAGE Checksumm_old: {1}\n '
                                        ' PAGE Checksumm_new: {2}\n '
                                        ' File: {3}\n'
                                    ).format(
                                        page,
                                        original_pgdata['files'][file][
                                            'md5_per_page'][page],
                                        restored_pgdata['files'][file][
                                            'md5_per_page'][page],
                                        os.path.join(
                                            restored_pgdata['pgdata'], file)
                                        )
                        for page in restored_pgdata['files'][file]['md5_per_page']:
                            if page not in original_pgdata['files'][file]['md5_per_page']:
                                error_message += '\n Extra page {0}\n File: {1}\n'.format(
                                    page,
                                    os.path.join(
                                        restored_pgdata['pgdata'], file))

            else:
                error_message += (
                    '\nFile disappearance.\n '
                    'File: {0}\n').format(
                    os.path.join(restored_pgdata['pgdata'], file)
                    )
                fail = True
        self.assertFalse(fail, error_message)

    def gdb_attach(self, pid):
        return GDBobj([str(pid)], self.verbose, attach=True)


class GdbException(Exception):
    def __init__(self, message=False):
        self.message = message

    def __str__(self):
        return '\n ERROR: {0}\n'.format(repr(self.message))


class GDBobj(ProbackupTest):
    def __init__(self, cmd, verbose, attach=False):
        self.verbose = verbose
        self.output = ''

        # Check gdb presense
        try:
            gdb_version, _ = subprocess.Popen(
                ['gdb', '--version'],
                stdout=subprocess.PIPE
            ).communicate()
        except OSError:
            raise GdbException("Couldn't find gdb on the path")

        self.base_cmd = [
            'gdb',
            '--interpreter',
            'mi2',
            ]

        if attach:
            self.cmd = self.base_cmd + ['--pid'] + cmd
        else:
            self.cmd = self.base_cmd + ['--args'] + cmd

        # Get version
        gdb_version_number = re.search(
            b"^GNU gdb [^\d]*(\d+)\.(\d)",
            gdb_version)
        self.major_version = int(gdb_version_number.group(1))
        self.minor_version = int(gdb_version_number.group(2))

        if self.verbose:
            print([' '.join(map(str, self.cmd))])

        self.proc = subprocess.Popen(
            self.cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
            universal_newlines=True
        )
        self.gdb_pid = self.proc.pid

        while True:
            line = self.get_line()

            if 'No such process' in line:
                raise GdbException(line)

            if not line.startswith('(gdb)'):
                pass
            else:
                break

    def get_line(self):
        line = self.proc.stdout.readline()
        self.output += line
        return line

    def kill(self):
        self.proc.kill()
        self.proc.wait()

    def set_breakpoint(self, location):

        result = self._execute('break ' + location)
        for line in result:
            if line.startswith('~"Breakpoint'):
                return

            elif line.startswith('=breakpoint-created'):
                return

            elif line.startswith('^error'): #or line.startswith('(gdb)'):
                break

            elif line.startswith('&"break'):
                pass

            elif line.startswith('&"Function'):
                raise GdbException(line)

            elif line.startswith('&"No line'):
                raise GdbException(line)

            elif line.startswith('~"Make breakpoint pending on future shared'):
                raise GdbException(line)

        raise GdbException(
            'Failed to set breakpoint.\n Output:\n {0}'.format(result)
        )

    def remove_all_breakpoints(self):

        result = self._execute('delete')
        for line in result:

            if line.startswith('^done'):
                return

        raise GdbException(
            'Failed to remove breakpoints.\n Output:\n {0}'.format(result)
        )

    def run_until_break(self):
        result = self._execute('run', False)
        for line in result:
            if line.startswith('*stopped,reason="breakpoint-hit"'):
                return
        raise GdbException(
            'Failed to run until breakpoint.\n'
        )

    def continue_execution_until_running(self):
        result = self._execute('continue')

        for line in result:
            if line.startswith('*running') or line.startswith('^running'):
                return
            if line.startswith('*stopped,reason="breakpoint-hit"'):
                continue
            if line.startswith('*stopped,reason="exited-normally"'):
                continue

        raise GdbException(
                'Failed to continue execution until running.\n'
            )

    def continue_execution_until_exit(self):
        result = self._execute('continue', False)

        for line in result:
            if line.startswith('*running'):
                continue
            if line.startswith('*stopped,reason="breakpoint-hit"'):
                continue
            if (
                line.startswith('*stopped,reason="exited') or
                line == '*stopped\n'
            ):
                return

        raise GdbException(
            'Failed to continue execution until exit.\n'
        )

    def continue_execution_until_error(self):
        result = self._execute('continue', False)

        for line in result:
            if line.startswith('^error'):
                return
            if line.startswith('*stopped,reason="exited'):
                return
            if line.startswith(
                '*stopped,reason="signal-received",signal-name="SIGABRT"'):
                return

        raise GdbException(
            'Failed to continue execution until error.\n')

    def continue_execution_until_break(self, ignore_count=0):
        if ignore_count > 0:
            result = self._execute(
                'continue ' + str(ignore_count),
                False
            )
        else:
            result = self._execute('continue', False)

        for line in result:
            if line.startswith('*stopped,reason="breakpoint-hit"'):
                return
            if line.startswith('*stopped,reason="exited-normally"'):
                break

        raise GdbException(
            'Failed to continue execution until break.\n')

    def stopped_in_breakpoint(self):
        while True:
            line = self.get_line()
            if self.verbose:
                print(line)
            if line.startswith('*stopped,reason="breakpoint-hit"'):
                return True
        return False

    def quit(self):
        self.proc.terminate()

    # use for breakpoint, run, continue
    def _execute(self, cmd, running=True):
        output = []
        self.proc.stdin.flush()
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()
        sleep(1)

        # look for command we just send
        while True:
            line = self.get_line()
            if self.verbose:
                print(repr(line))

            if cmd not in line:
                continue
            else:
                break

        while True:
            line = self.get_line()
            output += [line]
            if self.verbose:
                print(repr(line))
            if line.startswith('^done') or line.startswith('*stopped'):
                break
            if line.startswith('^error'):
                break
            if running and (line.startswith('*running') or line.startswith('^running')):
#            if running and line.startswith('*running'):
                break
        return output
