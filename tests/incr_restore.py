import os
import unittest
from .helpers.ptrack_helpers import ProbackupTest, ProbackupException
import subprocess
from datetime import datetime
import sys
from time import sleep
from datetime import datetime, timedelta
import hashlib
import shutil
import json
from testgres import QueryException, StartNodeException


module_name = 'incr_restore'


class IncrRestoreTest(ProbackupTest, unittest.TestCase):

    # @unittest.skip("skip")
    def test_basic_incr_restore(self):
        """incremental restore in CHECKSUM mode"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=50)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_basic_incr_restore_into_missing_directory(self):
        """"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=10)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgdata = self.pgdata_content(node.data_dir)

        node.cleanup()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_checksum_corruption_detection(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=10)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgdata = self.pgdata_content(node.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--incremental-mode=lsn"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        tblspace = self.get_tblspace_path(node, 'tblspace')
        some_directory = self.get_tblspace_path(node, 'some_directory')

        # stuff new destination with garbage
        self.restore_node(backup_dir, 'node', node, data_dir=some_directory)

        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=10, tablespace='tblspace')

        self.backup_node(backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=[
                "-j", "4", "--incremental-mode=checksum", "--force",
                "-T{0}={1}".format(tblspace, some_directory)])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_1(self):
        """recovery to target timeline"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            set_replication=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        tblspace = self.get_tblspace_path(node, 'tblspace')
        some_directory = self.get_tblspace_path(node, 'some_directory')

        self.restore_node(backup_dir, 'node', node, data_dir=some_directory)

        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=10, tablespace='tblspace')

        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(
            backup_dir, 'node', node, backup_type='delta', options=['--stream'])

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(
            backup_dir, 'node', node, backup_type='delta', options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_2(self):
        """
        If "--tablespace-mapping" option is used with incremental restore,
        then new directory must be empty.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            set_replication=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))

        # fill node1 with data
        out = self.restore_node(
            backup_dir, 'node', node,
            data_dir=node_1.data_dir,
            options=['--incremental-mode=checksum', '--force'])

        self.assertIn("WARNING: Backup catalog was initialized for system id", out)

        tblspace = self.get_tblspace_path(node, 'tblspace')
        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=5, tablespace='tblspace')

        node.safe_psql(
            'postgres',
            'vacuum')

        self.backup_node(backup_dir, 'node', node, backup_type='delta', options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        try:
            self.restore_node(
                backup_dir, 'node', node,
                data_dir=node_1.data_dir,
                options=['--incremental-mode=checksum', '-T{0}={1}'.format(tblspace, tblspace)])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because remapped directory is not empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Remapped tablespace destination is not empty',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        out = self.restore_node(
            backup_dir, 'node', node,
            data_dir=node_1.data_dir,
            options=[
                '--force', '--incremental-mode=checksum',
                '-T{0}={1}'.format(tblspace, tblspace)])

        pgdata_restored = self.pgdata_content(node_1.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_3(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.create_tblspace_in_node(node, 'tblspace1')
        node.pgbench_init(scale=10, tablespace='tblspace1')

        # take backup with tblspace1
        self.backup_node(backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        self.drop_tblspace(node, 'tblspace1')

        self.create_tblspace_in_node(node, 'tblspace2')
        node.pgbench_init(scale=10, tablespace='tblspace2')

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=[
                "-j", "4",
                "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_4(self):
        """
        Check that system ID mismatch is detected,
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.create_tblspace_in_node(node, 'tblspace1')
        node.pgbench_init(scale=10, tablespace='tblspace1')

        # take backup of node1 with tblspace1
        self.backup_node(backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        self.drop_tblspace(node, 'tblspace1')
        node.cleanup()

        # recreate node
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])
        node.slow_start()

        self.create_tblspace_in_node(node, 'tblspace1')
        node.pgbench_init(scale=10, tablespace='tblspace1')
        node.stop()

        try:
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4",
                    "--incremental-mode=checksum"])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because destination directory has wrong system id.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'WARNING: Backup catalog was initialized for system id',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))
            self.assertIn(
                'ERROR: Incremental restore is not allowed',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        out = self.restore_node(
            backup_dir, 'node', node,
            options=[
                "-j", "4", "--force",
                "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.expectedFailure
    @unittest.skip("skip")
    def test_incr_restore_with_tablespace_5(self):
        """
        More complicated case, we restore backup
        with tablespace, which we remap into directory
        with some old content, that belongs to an instance
        with different system id.
        """
        fname = self.id().split('.')[3]
        node1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node1'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node1)
        node1.slow_start()

        self.create_tblspace_in_node(node1, 'tblspace')
        node1.pgbench_init(scale=10, tablespace='tblspace')

        # take backup of node1 with tblspace
        self.backup_node(backup_dir, 'node', node1, options=['--stream'])
        pgdata = self.pgdata_content(node1.data_dir)

        node1.stop()

        # recreate node
        node2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node2'),
            set_replication=True,
            initdb_params=['--data-checksums'])
        node2.slow_start()

        self.create_tblspace_in_node(node2, 'tblspace')
        node2.pgbench_init(scale=10, tablespace='tblspace')
        node2.stop()

        tblspc1_path = self.get_tblspace_path(node1, 'tblspace')
        tblspc2_path = self.get_tblspace_path(node2, 'tblspace')

        out = self.restore_node(
            backup_dir, 'node', node1,
            options=[
                "-j", "4", "--force",
                "--incremental-mode=checksum",
                "-T{0}={1}".format(tblspc1_path, tblspc2_path)])

        # check that tblspc1_path is empty
        self.assertFalse(
            os.listdir(tblspc1_path),
            "Dir is not empty: '{0}'".format(tblspc1_path))

        pgdata_restored = self.pgdata_content(node1.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_6(self):
        """
        Empty pgdata, not empty tablespace
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=10, tablespace='tblspace')

        # take backup of node with tblspace
        self.backup_node(backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        node.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4",
                    "--incremental-mode=checksum"])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because there is running postmaster "
                "process in destination directory.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: PGDATA is empty, but tablespace destination is not',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        out = self.restore_node(
            backup_dir, 'node', node,
            options=[
                "-j", "4", "--force",
                "--incremental-mode=checksum"])

        self.assertIn(
            "INFO: Destination directory and tablespace directories are empty, "
            "disable incremental restore", out)

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_restore_with_tablespace_7(self):
        """
        Restore backup without tablespace into
        PGDATA with tablespace.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        # take backup of node with tblspace
        self.backup_node(backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=5, tablespace='tblspace')
        node.stop()

#        try:
#            self.restore_node(
#                backup_dir, 'node', node,
#                options=[
#                    "-j", "4",
#                    "--incremental-mode=checksum"])
#            # we should die here because exception is what we expect to happen
#            self.assertEqual(
#                1, 0,
#                "Expecting Error because there is running postmaster "
#                "process in destination directory.\n "
#                "Output: {0} \n CMD: {1}".format(
#                    repr(self.output), self.cmd))
#        except ProbackupException as e:
#            self.assertIn(
#                'ERROR: PGDATA is empty, but tablespace destination is not',
#                e.message,
#                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
#                    repr(e.message), self.cmd))

        out = self.restore_node(
            backup_dir, 'node', node,
            options=[
                "-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_basic_incr_restore_sanity(self):
        """recovery to target timeline"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            set_replication=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        try:
            self.restore_node(
                backup_dir, 'node', node,
                options=["-j", "4", "--incremental-mode=checksum"])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because there is running postmaster "
                "process in destination directory.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'WARNING: Postmaster with pid',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))
            self.assertIn(
                'ERROR: Incremental restore is not allowed',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))

        try:
            self.restore_node(
                backup_dir, 'node', node_1, data_dir=node_1.data_dir,
                options=["-j", "4", "--incremental-mode=checksum"])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because destination directory has wrong system id.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'WARNING: Backup catalog was initialized for system id',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))
            self.assertIn(
                'ERROR: Incremental restore is not allowed',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_checksum_restore(self):
        """
                        /----C-----D
        ------A----B---*--------X

        X - is instance, we want to return it to C state.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=50)
        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        xid = node.safe_psql(
            'postgres',
            'select txid_current()').decode('utf-8').rstrip()

        # --A-----B--------X
        pgbench = node.pgbench(options=['-T', '30', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        node.stop(['-m', 'immediate', '-D', node.data_dir])

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))
        node_1.cleanup()

        self.restore_node(
            backup_dir, 'node', node_1, data_dir=node_1.data_dir,
            options=[
                '--recovery-target-action=promote',
                '--recovery-target-xid={0}'.format(xid)])

        self.set_auto_conf(node_1, {'port': node_1.port})
        node_1.slow_start()

        #               /--
        # --A-----B----*----X
        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        #               /--C
        # --A-----B----*----X
        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')

        #               /--C------
        # --A-----B----*----X
        pgbench = node_1.pgbench(options=['-T', '50', '-c', '1'])
        pgbench.wait()

        #               /--C------D
        # --A-----B----*----X
        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')

        pgdata = self.pgdata_content(node_1.data_dir)

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)

        self.set_auto_conf(node, {'port': node.port})
        node.slow_start()

        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)


    # @unittest.skip("skip")
    def test_incr_lsn_restore(self):
        """
                        /----C-----D
        ------A----B---*--------X

        X - is instance, we want to return it to C state.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=50)
        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        pgbench = node.pgbench(options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        xid = node.safe_psql(
            'postgres',
            'select txid_current()').decode('utf-8').rstrip()

        # --A-----B--------X
        pgbench = node.pgbench(options=['-T', '30', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        node.stop(['-m', 'immediate', '-D', node.data_dir])

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))
        node_1.cleanup()

        self.restore_node(
            backup_dir, 'node', node_1, data_dir=node_1.data_dir,
            options=[
                '--recovery-target-action=promote',
                '--recovery-target-xid={0}'.format(xid)])

        self.set_auto_conf(node_1, {'port': node_1.port})
        node_1.slow_start()

        #               /--
        # --A-----B----*----X
        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        #               /--C
        # --A-----B----*----X
        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')

        #               /--C------
        # --A-----B----*----X
        pgbench = node_1.pgbench(options=['-T', '50', '-c', '1'])
        pgbench.wait()

        #               /--C------D
        # --A-----B----*----X
        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')

        pgdata = self.pgdata_content(node_1.data_dir)

        self.restore_node(
            backup_dir, 'node', node, options=["-j", "4", "--incremental-mode=lsn"])

        pgdata_restored = self.pgdata_content(node.data_dir)

        self.set_auto_conf(node, {'port': node.port})
        node.slow_start()

        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_incr_lsn_sanity(self):
        """
                /----A-----B
        F------*--------X

        X - is instance, we want to return it to state B.
        fail is expected behaviour in case of lsn restore.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node)
        node.pgbench_init(scale=10)

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))
        node_1.cleanup()

        self.restore_node(
            backup_dir, 'node', node_1, data_dir=node_1.data_dir)

        self.set_auto_conf(node_1, {'port': node_1.port})
        node_1.slow_start()

        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='full')

        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')

        node.stop()

        try:
            self.restore_node(
                backup_dir, 'node', node, data_dir=node.data_dir,
                options=["-j", "4", "--incremental-mode=lsn"])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because incremental restore in lsn mode is impossible\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Cannot perform incremental restore of "
                "backup chain {0} in 'lsn' mode".format(page_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

        # @unittest.skip("skip")
    def test_incr_checksum_sanity(self):
        """
                /----A-----B
        F------*--------X

        X - is instance, we want to return it to state B.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node)
        node.pgbench_init(scale=20)

        node_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_1'))
        node_1.cleanup()

        self.restore_node(
            backup_dir, 'node', node_1, data_dir=node_1.data_dir)

        self.set_auto_conf(node_1, {'port': node_1.port})
        node_1.slow_start()

        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='full')

        pgbench = node_1.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(backup_dir, 'node', node_1,
            data_dir=node_1.data_dir, backup_type='page')
        pgdata = self.pgdata_content(node_1.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, data_dir=node.data_dir,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)

        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)


        # @unittest.skip("skip")
    def test_incr_checksum_corruption_detection(self):
        """
        check that corrupted page got detected and replaced
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
#            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node)
        node.pgbench_init(scale=20)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node,
            data_dir=node.data_dir, backup_type='full')

        heap_path = node.safe_psql(
            "postgres",
            "select pg_relation_filepath('pgbench_accounts')").decode('utf-8').rstrip()

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(backup_dir, 'node', node,
            data_dir=node.data_dir, backup_type='page')

        pgdata = self.pgdata_content(node.data_dir)

        node.stop()

        path = os.path.join(node.data_dir, heap_path)
        with open(path, "rb+", 0) as f:
                f.seek(22000)
                f.write(b"bla")
                f.flush()
                f.close

        self.restore_node(
            backup_dir, 'node', node, data_dir=node.data_dir,
            options=["-j", "4", "--incremental-mode=checksum"])

        pgdata_restored = self.pgdata_content(node.data_dir)

        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

        # @unittest.skip("skip")
    def test_incr_lsn_corruption_detection(self):
        """
        check that corrupted page got detected and replaced
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node)
        node.pgbench_init(scale=20)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        self.backup_node(backup_dir, 'node', node,
            data_dir=node.data_dir, backup_type='full')

        heap_path = node.safe_psql(
            "postgres",
            "select pg_relation_filepath('pgbench_accounts')").decode('utf-8').rstrip()

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(backup_dir, 'node', node,
            data_dir=node.data_dir, backup_type='page')

        pgdata = self.pgdata_content(node.data_dir)

        node.stop()

        path = os.path.join(node.data_dir, heap_path)
        with open(path, "rb+", 0) as f:
                f.seek(22000)
                f.write(b"bla")
                f.flush()
                f.close

        self.restore_node(
            backup_dir, 'node', node, data_dir=node.data_dir,
            options=["-j", "4", "--incremental-mode=lsn"])

        pgdata_restored = self.pgdata_content(node.data_dir)

        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_restore_multiple_external(self):
        """check that cmdline has priority over config"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        external_dir1 = self.get_tblspace_path(node, 'external_dir1')
        external_dir2 = self.get_tblspace_path(node, 'external_dir2')

        # FULL backup
        node.pgbench_init(scale=20)
        self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4"])

        # fill external directories with data
        self.restore_node(
            backup_dir, 'node', node,
            data_dir=external_dir1, options=["-j", "4"])

        self.restore_node(
            backup_dir, 'node', node,
            data_dir=external_dir2, options=["-j", "4"])

        self.set_config(
            backup_dir, 'node',
            options=['-E{0}{1}{2}'.format(
                external_dir1, self.EXTERNAL_DIRECTORY_DELIMITER, external_dir2)])

        # cmdline option MUST override options in config
        self.backup_node(
            backup_dir, 'node', node,
            backup_type='full', options=["-j", "4"])

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # cmdline option MUST override options in config
        self.backup_node(
            backup_dir, 'node', node,
            backup_type='page', options=["-j", "4"])

        pgdata = self.pgdata_content(
            node.base_dir, exclude_dirs=['logs'])

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", '--incremental-mode=checksum'])

        pgdata_restored = self.pgdata_content(
            node.base_dir, exclude_dirs=['logs'])
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_lsn_restore_multiple_external(self):
        """check that cmdline has priority over config"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        external_dir1 = self.get_tblspace_path(node, 'external_dir1')
        external_dir2 = self.get_tblspace_path(node, 'external_dir2')

        # FULL backup
        node.pgbench_init(scale=20)
        self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4"])

        # fill external directories with data
        self.restore_node(
            backup_dir, 'node', node,
            data_dir=external_dir1, options=["-j", "4"])

        self.restore_node(
            backup_dir, 'node', node,
            data_dir=external_dir2, options=["-j", "4"])

        self.set_config(
            backup_dir, 'node',
            options=['-E{0}{1}{2}'.format(
                external_dir1, self.EXTERNAL_DIRECTORY_DELIMITER, external_dir2)])

        # cmdline option MUST override options in config
        self.backup_node(
            backup_dir, 'node', node,
            backup_type='full', options=["-j", "4"])

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # cmdline option MUST override options in config
        self.backup_node(
            backup_dir, 'node', node,
            backup_type='page', options=["-j", "4"])

        pgdata = self.pgdata_content(
            node.base_dir, exclude_dirs=['logs'])

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        node.stop()

        self.restore_node(
            backup_dir, 'node', node,
            options=["-j", "4", '--incremental-mode=lsn'])

        pgdata_restored = self.pgdata_content(
            node.base_dir, exclude_dirs=['logs'])
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_lsn_restore_backward(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'],
            pg_options={'wal_log_hints': 'on', 'hot_standby': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        node.pgbench_init(scale=2)
        full_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4"])

        full_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='page', options=["-j", "4"])

        page_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        delta_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta', options=["-j", "4"])

        delta_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=full_id,
            options=[
                "-j", "4",
                '--incremental-mode=lsn',
                '--recovery-target=immediate',
                '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(full_pgdata, pgdata_restored)

        node.slow_start(replica=True)
        node.stop()

        try:
            self.restore_node(
                backup_dir, 'node', node, backup_id=page_id,
                options=[
                    "-j", "4", '--incremental-mode=lsn',
                    '--recovery-target=immediate', '--recovery-target-action=pause'])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because incremental restore in lsn mode is impossible\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "Cannot perform incremental restore of backup chain",
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(
            backup_dir, 'node', node, backup_id=page_id,
            options=[
                "-j", "4", '--incremental-mode=checksum',
                '--recovery-target=immediate', '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(page_pgdata, pgdata_restored)

        node.slow_start(replica=True)
        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=delta_id,
            options=[
                "-j", "4",
                '--incremental-mode=lsn',
                '--recovery-target=immediate',
                '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(delta_pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_checksum_restore_backward(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'],
            pg_options={
                'hot_standby': 'on'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        node.pgbench_init(scale=20)
        full_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4"])

        full_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        page_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='page', options=["-j", "4"])

        page_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        delta_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta', options=["-j", "4"])

        delta_pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=full_id,
            options=[
                "-j", "4",
                '--incremental-mode=checksum',
                '--recovery-target=immediate',
                '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(full_pgdata, pgdata_restored)

        node.slow_start(replica=True)
        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=page_id,
            options=[
                "-j", "4",
                '--incremental-mode=checksum',
                '--recovery-target=immediate',
                '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(page_pgdata, pgdata_restored)

        node.slow_start(replica=True)
        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=delta_id,
            options=[
                "-j", "4",
                '--incremental-mode=checksum',
                '--recovery-target=immediate',
                '--recovery-target-action=pause'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(delta_pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_make_replica_via_incr_checksum_restore(self):
        """
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        master = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'master'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        if self.get_version(master) < self.version_to_num('9.6.0'):
            self.del_test_dir(module_name, fname)
            return unittest.skip(
                'Skipped because backup from replica is not supported in PG 9.5')

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', master)
        self.set_archiving(backup_dir, 'node', master, replica=True)
        master.slow_start()

        replica = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'replica'))
        replica.cleanup()

        master.pgbench_init(scale=20)

        self.backup_node(backup_dir, 'node', master)

        self.restore_node(
            backup_dir, 'node', replica, options=['-R'])

        # Settings for Replica
        self.set_replica(master, replica, synchronous=False)

        replica.slow_start(replica=True)

        pgbench = master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # PROMOTIONS
        replica.promote()
        new_master = replica

        # old master is going a bit further
        old_master = master
        pgbench = old_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()
        old_master.stop()

        pgbench = new_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # take backup from new master
        self.backup_node(
            backup_dir, 'node', new_master,
            data_dir=new_master.data_dir, backup_type='page')

        # restore old master as replica
        self.restore_node(
            backup_dir, 'node', old_master, data_dir=old_master.data_dir,
            options=['-R', '--incremental-mode=checksum'])

        self.set_replica(new_master, old_master, synchronous=True)

        old_master.slow_start(replica=True)

        pgbench = new_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_make_replica_via_incr_lsn_restore(self):
        """
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        master = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'master'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        if self.get_version(master) < self.version_to_num('9.6.0'):
            self.del_test_dir(module_name, fname)
            return unittest.skip(
                'Skipped because backup from replica is not supported in PG 9.5')

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', master)
        self.set_archiving(backup_dir, 'node', master, replica=True)
        master.slow_start()

        replica = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'replica'))
        replica.cleanup()

        master.pgbench_init(scale=20)

        self.backup_node(backup_dir, 'node', master)

        self.restore_node(
            backup_dir, 'node', replica, options=['-R'])

        # Settings for Replica
        self.set_replica(master, replica, synchronous=False)

        replica.slow_start(replica=True)

        pgbench = master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # PROMOTIONS
        replica.promote()
        new_master = replica

        # old master is going a bit further
        old_master = master
        pgbench = old_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()
        old_master.stop()

        pgbench = new_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # take backup from new master
        self.backup_node(
            backup_dir, 'node', new_master,
            data_dir=new_master.data_dir, backup_type='page')

        # restore old master as replica
        self.restore_node(
            backup_dir, 'node', old_master, data_dir=old_master.data_dir,
            options=['-R', '--incremental-mode=lsn'])

        self.set_replica(new_master, old_master, synchronous=True)

        old_master.slow_start(replica=True)

        pgbench = new_master.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_checksum_long_xact(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            'postgres',
            'create extension pageinspect')

        # FULL backup
        con = node.connect("postgres")
        con.execute("CREATE TABLE t1 (a int)")
        con.commit()


        con.execute("INSERT INTO t1 values (1)")
        con.commit()

        # leave uncommited
        con2 = node.connect("postgres")
        con.execute("INSERT INTO t1 values (2)")
        con2.execute("INSERT INTO t1 values (3)")

        full_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4", "--stream"])

        self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])

        con.commit()

        node.safe_psql(
            'postgres',
            'select * from t1')

        con2.commit()
        node.safe_psql(
            'postgres',
            'select * from t1')

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=full_id,
            options=["-j", "4", '--incremental-mode=checksum'])

        node.slow_start()

        self.assertEqual(
            node.safe_psql(
                'postgres',
                'select count(*) from t1').decode('utf-8').rstrip(),
            '1')

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    # This test will pass with Enterprise
    # because it has checksums enabled by default
    @unittest.skipIf(ProbackupTest.enterprise, 'skip')
    def test_incr_lsn_long_xact_1(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            'postgres',
            'create extension pageinspect')

        # FULL backup
        con = node.connect("postgres")
        con.execute("CREATE TABLE t1 (a int)")
        con.commit()


        con.execute("INSERT INTO t1 values (1)")
        con.commit()

        # leave uncommited
        con2 = node.connect("postgres")
        con.execute("INSERT INTO t1 values (2)")
        con2.execute("INSERT INTO t1 values (3)")

        full_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4", "--stream"])

        self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])

        con.commit()

        # when does LSN gets stamped when checksum gets updated ?
        node.safe_psql(
            'postgres',
            'select * from t1')

        con2.commit()
        node.safe_psql(
            'postgres',
            'select * from t1')

        node.stop()

        try:
            self.restore_node(
                backup_dir, 'node', node, backup_id=full_id,
                options=["-j", "4", '--incremental-mode=lsn'])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because incremental restore in lsn mode is impossible\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Incremental restore in 'lsn' mode require data_checksums to be "
                "enabled in destination data directory",
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_lsn_long_xact_2(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'],
            pg_options={
                'full_page_writes': 'off',
                'wal_log_hints': 'off'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            'postgres',
            'create extension pageinspect')

        # FULL backup
        con = node.connect("postgres")
        con.execute("CREATE TABLE t1 (a int)")
        con.commit()


        con.execute("INSERT INTO t1 values (1)")
        con.commit()

        # leave uncommited
        con2 = node.connect("postgres")
        con.execute("INSERT INTO t1 values (2)")
        con2.execute("INSERT INTO t1 values (3)")

        full_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="full", options=["-j", "4", "--stream"])

        self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])

#        print(node.safe_psql(
#            'postgres',
#            "select * from page_header(get_raw_page('t1', 0))"))

        con.commit()

        # when does LSN gets stamped when checksum gets updated ?
        node.safe_psql(
            'postgres',
            'select * from t1')

#        print(node.safe_psql(
#            'postgres',
#            "select * from page_header(get_raw_page('t1', 0))"))

        con2.commit()
        node.safe_psql(
            'postgres',
            'select * from t1')

#        print(node.safe_psql(
#            'postgres',
#            "select * from page_header(get_raw_page('t1', 0))"))

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=full_id,
            options=["-j", "4", '--incremental-mode=lsn'])

        node.slow_start()

        self.assertEqual(
            node.safe_psql(
                'postgres',
                'select count(*) from t1').decode('utf-8').rstrip(),
            '1')

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_restore_zero_size_file_checksum(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        fullpath = os.path.join(node.data_dir, 'simple_file')
        with open(fullpath, "w+b", 0) as f:
            f.flush()
            f.close

        # FULL backup
        id1 = self.backup_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--stream"])

        pgdata1 = self.pgdata_content(node.data_dir)

        with open(fullpath, "rb+", 0) as f:
            f.seek(9000)
            f.write(b"bla")
            f.flush()
            f.close

        id2 = self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])
        pgdata2 = self.pgdata_content(node.data_dir)

        with open(fullpath, "w") as f:
            f.close()

        id3 = self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])
        pgdata3 = self.pgdata_content(node.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=id1,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata1, pgdata_restored)

        self.restore_node(
            backup_dir, 'node', node, backup_id=id2,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata2, pgdata_restored)

        self.restore_node(
            backup_dir, 'node', node, backup_id=id3,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata3, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_incr_restore_zero_size_file_lsn(self):
        """
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        fullpath = os.path.join(node.data_dir, 'simple_file')
        with open(fullpath, "w+b", 0) as f:
            f.flush()
            f.close

        # FULL backup
        id1 = self.backup_node(
            backup_dir, 'node', node,
            options=["-j", "4", "--stream"])

        pgdata1 = self.pgdata_content(node.data_dir)

        with open(fullpath, "rb+", 0) as f:
            f.seek(9000)
            f.write(b"bla")
            f.flush()
            f.close

        id2 = self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])
        pgdata2 = self.pgdata_content(node.data_dir)

        with open(fullpath, "w") as f:
            f.close()

        id3 = self.backup_node(
            backup_dir, 'node', node,
            backup_type="delta", options=["-j", "4", "--stream"])
        pgdata3 = self.pgdata_content(node.data_dir)

        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=id1,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata1, pgdata_restored)

        node.slow_start()
        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=id2,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata2, pgdata_restored)

        node.slow_start()
        node.stop()

        self.restore_node(
            backup_dir, 'node', node, backup_id=id3,
            options=["-j", "4", '-I', 'checksum'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata3, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_incremental_partial_restore_exclude_checksum(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').rstrip()

        db_list_splitted = db_list_raw.splitlines()

        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        node.pgbench_init(scale=20)

        # FULL backup
        self.backup_node(backup_dir, 'node', node)
        pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # PAGE backup
        backup_id = self.backup_node(backup_dir, 'node', node, backup_type='page')

        # restore FULL backup into second node2
        node1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node1'))
        node1.cleanup()

        node2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node2'))
        node2.cleanup()

        # restore some data into node2
        self.restore_node(backup_dir, 'node', node2)

        # partial restore backup into node1
        self.restore_node(
            backup_dir, 'node',
            node1, options=[
                "--db-exclude=db1",
                "--db-exclude=db5"])

        pgdata1 = self.pgdata_content(node1.data_dir)

        # partial incremental restore backup into node2
        self.restore_node(
            backup_dir, 'node',
            node2, options=[
                "--db-exclude=db1",
                "--db-exclude=db5",
                "-I", "checksum"])

        pgdata2 = self.pgdata_content(node2.data_dir)

        self.compare_pgdata(pgdata1, pgdata2)

        self.set_auto_conf(node2, {'port': node2.port})

        node2.slow_start()

        node2.safe_psql(
            'postgres',
            'select 1')

        try:
            node2.safe_psql(
                'db1',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node2.safe_psql(
                'db5',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_incremental_partial_restore_exclude_lsn(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').rstrip()

        db_list_splitted = db_list_raw.splitlines()

        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        node.pgbench_init(scale=20)

        # FULL backup
        self.backup_node(backup_dir, 'node', node)
        pgdata = self.pgdata_content(node.data_dir)

        pgbench = node.pgbench(options=['-T', '10', '-c', '1'])
        pgbench.wait()

        # PAGE backup
        backup_id = self.backup_node(backup_dir, 'node', node, backup_type='page')

        node.stop()

        # restore FULL backup into second node2
        node1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node1'))
        node1.cleanup()

        node2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node2'))
        node2.cleanup()

        # restore some data into node2
        self.restore_node(backup_dir, 'node', node2)

        # partial restore backup into node1
        self.restore_node(
            backup_dir, 'node',
            node1, options=[
                "--db-exclude=db1",
                "--db-exclude=db5"])

        pgdata1 = self.pgdata_content(node1.data_dir)

        # partial incremental restore backup into node2
        node2.port = node.port
        node2.slow_start()
        node2.stop()
        self.restore_node(
            backup_dir, 'node',
            node2, options=[
                "--db-exclude=db1",
                "--db-exclude=db5",
                "-I", "lsn"])

        pgdata2 = self.pgdata_content(node2.data_dir)

        self.compare_pgdata(pgdata1, pgdata2)

        self.set_auto_conf(node2, {'port': node2.port})

        node2.slow_start()

        node2.safe_psql(
            'postgres',
            'select 1')

        try:
            node2.safe_psql(
                'db1',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node2.safe_psql(
                'db5',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_incremental_partial_restore_exclude_tablespace_checksum(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # cat_version = node.get_control_data()["Catalog version number"]
        # version_specific_dir = 'PG_' + node.major_version_str + '_' + cat_version

        # PG_10_201707211
        # pg_tblspc/33172/PG_9.5_201510051/16386/

        self.create_tblspace_in_node(node, 'somedata')

        node_tablespace = self.get_tblspace_path(node, 'somedata')

        tbl_oid = node.safe_psql(
            'postgres',
            "SELECT oid "
            "FROM pg_tablespace "
            "WHERE spcname = 'somedata'").rstrip()

        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0} tablespace somedata'.format(i))

        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').rstrip()

        db_list_splitted = db_list_raw.splitlines()

        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)

        # node1
        node1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node1'))
        node1.cleanup()
        node1_tablespace = self.get_tblspace_path(node1, 'somedata')

        # node2
        node2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node2'))
        node2.cleanup()
        node2_tablespace = self.get_tblspace_path(node2, 'somedata')

        # in node2 restore full backup
        self.restore_node(
            backup_dir, 'node',
            node2, options=[
                "-T", "{0}={1}".format(
                    node_tablespace, node2_tablespace)])

        # partial restore into node1
        self.restore_node(
            backup_dir, 'node',
            node1, options=[
                "--db-exclude=db1",
                "--db-exclude=db5",
                "-T", "{0}={1}".format(
                    node_tablespace, node1_tablespace)])

        pgdata1 = self.pgdata_content(node1.data_dir)

        # partial incremental restore into node2
        try:
            self.restore_node(
                backup_dir, 'node',
                node2, options=[
                    "-I", "checksum",
                    "--db-exclude=db1",
                    "--db-exclude=db5",
                    "-T", "{0}={1}".format(
                        node_tablespace, node2_tablespace)])
                        # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because remapped tablespace contain old data .\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Remapped tablespace destination is not empty:',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(
            backup_dir, 'node',
            node2, options=[
                "-I", "checksum", "--force",
                "--db-exclude=db1",
                "--db-exclude=db5",
                "-T", "{0}={1}".format(
                    node_tablespace, node2_tablespace)])

        pgdata2 = self.pgdata_content(node2.data_dir)

        self.compare_pgdata(pgdata1, pgdata2)

        self.set_auto_conf(node2, {'port': node2.port})
        node2.slow_start()

        node2.safe_psql(
            'postgres',
            'select 1')

        try:
            node2.safe_psql(
                'db1',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node2.safe_psql(
                'db5',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_incremental_pg_filenode_map(self):
        """
        https://github.com/postgrespro/pg_probackup/issues/320
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node1'),
            initdb_params=['--data-checksums'])
        node1.cleanup()

        node.pgbench_init(scale=5)

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)

        # in node1 restore full backup
        self.restore_node(backup_dir, 'node', node1)
        self.set_auto_conf(node1, {'port': node1.port})
        node1.slow_start()

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1'])

        pgbench = node1.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1'])

        node.safe_psql(
            'postgres',
            'reindex index pg_type_oid_index')

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)

        node1.stop()

        # incremental restore into node1
        self.restore_node(backup_dir, 'node', node1, options=["-I", "checksum"])

        self.set_auto_conf(node1, {'port': node1.port})
        node1.slow_start()

        node1.safe_psql(
            'postgres',
            'select 1')

        # Clean after yourself
        self.del_test_dir(module_name, fname)

# check that MinRecPoint and BackupStartLsn are correctly used in case of --incrementa-lsn


    # @unittest.skip("skip")
    def test_incr_restore_issue_313(self):
        """
        Check that failed incremental restore can be restarted
        """
        fname = self.id().split('.')[3]

        node = self.make_simple_node(
            base_dir = os.path.join(module_name, fname, 'node'),
            set_replication = True,
            initdb_params = ['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale = 50)

        full_backup_id = self.backup_node(backup_dir, 'node', node, backup_type='full')

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        last_backup_id = self.backup_node(backup_dir, 'node', node, backup_type='delta')

        pgdata = self.pgdata_content(node.data_dir)
        node.cleanup()

        self.restore_node(backup_dir, 'node', node, backup_id=full_backup_id)

        count = 0
        filelist = self.get_backup_filelist(backup_dir, 'node', last_backup_id)
        for file in filelist:
            # count only nondata files
            if int(filelist[file]['is_datafile']) == 0 and int(filelist[file]['size']) > 0:
                count += 1

        gdb = self.restore_node(backup_dir, 'node', node, gdb=True,
            backup_id=last_backup_id, options=['--progress', '--incremental-mode=checksum'])
        gdb.verbose = False
        gdb.set_breakpoint('restore_non_data_file')
        gdb.run_until_break()
        gdb.continue_execution_until_break(count - 2)
        gdb.quit()

        try:
            node.slow_start()
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because backup is not fully restored")
        except StartNodeException as e:
            self.assertIn(
                'Cannot start node',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        with open(os.path.join(node.logs_dir, 'postgresql.log'), 'r') as f:
            self.assertIn(
                "postgres: could not find the database system",
                f.read())

        self.restore_node(backup_dir, 'node', node,
            backup_id=last_backup_id, options=['--progress', '--incremental-mode=checksum'])
        node.slow_start()

        self.compare_pgdata(pgdata, self.pgdata_content(node.data_dir))

        # Clean after yourself
        node.stop()
        self.del_test_dir(module_name, fname)
