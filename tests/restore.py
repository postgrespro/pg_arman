import os
import unittest
from .helpers.ptrack_helpers import ProbackupTest, ProbackupException
import subprocess
import sys
from time import sleep
from datetime import datetime, timedelta
import hashlib
import shutil
import json
from shutil import copyfile
from testgres import QueryException, StartNodeException
from stat import S_ISDIR


module_name = 'restore'


class RestoreTest(ProbackupTest, unittest.TestCase):

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_restore_full_to_latest(self):
        """recovery to latest from full backup"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()
        before = node.execute("postgres", "SELECT * FROM pgbench_branches")
        backup_id = self.backup_node(backup_dir, 'node', node)

        node.stop()
        node.cleanup()

        # 1 - Test recovery from latest
        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        # 2 - Test that recovery.conf was created
        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')
        self.assertEqual(os.path.isfile(recovery_conf), True)

        node.slow_start()

        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_full_page_to_latest(self):
        """recovery to latest from full + page backups"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type="page")

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_specific_timeline(self):
        """recovery to target timeline"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        backup_id = self.backup_node(backup_dir, 'node', node)

        target_tli = int(
            node.get_control_data()["Latest checkpoint's TimeLineID"])
        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            options=['-T', '10', '-c', '2', '--no-vacuum'])
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node)

        node.stop()
        node.cleanup()

        # Correct Backup must be choosen for restore
        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", "--timeline={0}".format(target_tli)]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        recovery_target_timeline = self.get_recovery_conf(
            node)["recovery_target_timeline"]
        self.assertEqual(int(recovery_target_timeline), target_tli)

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_time(self):
        """recovery to target time"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={'TimeZone': 'Europe/Moscow'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        backup_id = self.backup_node(backup_dir, 'node', node)

        target_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--time={0}'.format(target_time),
                    "--recovery-target-action=promote"
                    ]
                ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_xid_inclusive(self):
        """recovery to target xid"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        with node.connect("postgres") as con:
            con.execute("CREATE TABLE tbl0005 (a text)")
            con.commit()

        backup_id = self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        before = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        with node.connect("postgres") as con:
            res = con.execute("INSERT INTO tbl0005 VALUES ('inserted') RETURNING (xmin)")
            con.commit()
            target_xid = res[0][0]

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--xid={0}'.format(target_xid),
                    "--recovery-target-action=promote"]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)
        self.assertEqual(
            len(node.execute("postgres", "SELECT * FROM tbl0005")), 1)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_xid_not_inclusive(self):
        """recovery with target inclusive false"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        with node.connect("postgres") as con:
            con.execute("CREATE TABLE tbl0005 (a text)")
            con.commit()

        backup_id = self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")
        with node.connect("postgres") as con:
            result = con.execute("INSERT INTO tbl0005 VALUES ('inserted') RETURNING (xmin)")
            con.commit()
            target_xid = result[0][0]

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4",
                    '--xid={0}'.format(target_xid),
                    "--inclusive=false",
                    "--recovery-target-action=promote"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)
        self.assertEqual(
            len(node.execute("postgres", "SELECT * FROM tbl0005")), 0)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_lsn_inclusive(self):
        """recovery to target lsn"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        if self.get_version(node) < self.version_to_num('10.0'):
            self.del_test_dir(module_name, fname)
            return

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        with node.connect("postgres") as con:
            con.execute("CREATE TABLE tbl0005 (a int)")
            con.commit()

        backup_id = self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        before = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        with node.connect("postgres") as con:
            con.execute("INSERT INTO tbl0005 VALUES (1)")
            con.commit()
            res = con.execute("SELECT pg_current_wal_lsn()")
            con.commit()
            con.execute("INSERT INTO tbl0005 VALUES (2)")
            con.commit()
            xlogid, xrecoff = res[0][0].split('/')
            xrecoff = hex(int(xrecoff, 16) + 1)[2:]
            target_lsn = "{0}/{1}".format(xlogid, xrecoff)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--lsn={0}'.format(target_lsn),
                    "--recovery-target-action=promote"]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        after = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)
        self.assertEqual(
            len(node.execute("postgres", "SELECT * FROM tbl0005")), 2)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_to_lsn_not_inclusive(self):
        """recovery to target lsn"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        if self.get_version(node) < self.version_to_num('10.0'):
            self.del_test_dir(module_name, fname)
            return

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=2)
        with node.connect("postgres") as con:
            con.execute("CREATE TABLE tbl0005 (a int)")
            con.commit()

        backup_id = self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        before = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        with node.connect("postgres") as con:
            con.execute("INSERT INTO tbl0005 VALUES (1)")
            con.commit()
            res = con.execute("SELECT pg_current_wal_lsn()")
            con.commit()
            con.execute("INSERT INTO tbl0005 VALUES (2)")
            con.commit()
            xlogid, xrecoff = res[0][0].split('/')
            xrecoff = hex(int(xrecoff, 16) + 1)[2:]
            target_lsn = "{0}/{1}".format(xlogid, xrecoff)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "--inclusive=false",
                    "-j", "4", '--lsn={0}'.format(target_lsn),
                    "--recovery-target-action=promote"]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        after = node.safe_psql("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)
        self.assertEqual(
            len(node.execute("postgres", "SELECT * FROM tbl0005")), 1)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_full_ptrack_archive(self):
        """recovery to latest from archive full+ptrack backups"""
        if not self.ptrack:
            return unittest.skip('Skipped because ptrack support is disabled')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            ptrack_enable=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            "postgres",
            "CREATE EXTENSION ptrack")

        node.pgbench_init(scale=2)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type="ptrack")

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_ptrack(self):
        """recovery to latest from archive full+ptrack+ptrack backups"""
        if not self.ptrack:
            return unittest.skip('Skipped because ptrack support is disabled')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            ptrack_enable=True)

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            "postgres",
            "CREATE EXTENSION ptrack")

        node.pgbench_init(scale=2)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        self.backup_node(backup_dir, 'node', node, backup_type="ptrack")

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type="ptrack")

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_full_ptrack_stream(self):
        """recovery in stream mode to latest from full + ptrack backups"""
        if not self.ptrack:
            return unittest.skip('Skipped because ptrack support is disabled')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            ptrack_enable=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            "postgres",
            "CREATE EXTENSION ptrack")

        node.pgbench_init(scale=2)

        self.backup_node(backup_dir, 'node', node, options=["--stream"])

        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="ptrack", options=["--stream"])

        before = node.execute("postgres", "SELECT * FROM pgbench_branches")

        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        after = node.execute("postgres", "SELECT * FROM pgbench_branches")
        self.assertEqual(before, after)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_full_ptrack_under_load(self):
        """
        recovery to latest from full + ptrack backups
        with loads when ptrack backup do
        """
        if not self.ptrack:
            return unittest.skip('Skipped because ptrack support is disabled')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            ptrack_enable=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            "postgres",
            "CREATE EXTENSION ptrack")

        node.pgbench_init(scale=2)

        self.backup_node(backup_dir, 'node', node)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            options=["-c", "4", "-T", "8"]
        )

        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="ptrack", options=["--stream"])

        pgbench.wait()
        pgbench.stdout.close()

        bbalance = node.execute(
            "postgres", "SELECT sum(bbalance) FROM pgbench_branches")
        delta = node.execute(
            "postgres", "SELECT sum(delta) FROM pgbench_history")

        self.assertEqual(bbalance, delta)
        node.stop()
        node.cleanup()

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        bbalance = node.execute(
            "postgres", "SELECT sum(bbalance) FROM pgbench_branches")
        delta = node.execute(
            "postgres", "SELECT sum(delta) FROM pgbench_history")
        self.assertEqual(bbalance, delta)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_full_under_load_ptrack(self):
        """
        recovery to latest from full + page backups
        with loads when full backup do
        """
        if not self.ptrack:
            return unittest.skip('Skipped because ptrack support is disabled')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            ptrack_enable=True,
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.safe_psql(
            "postgres",
            "CREATE EXTENSION ptrack")

        # wal_segment_size = self.guc_wal_segment_size(node)
        node.pgbench_init(scale=2)

        pgbench = node.pgbench(
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            options=["-c", "4", "-T", "8"]
        )

        self.backup_node(backup_dir, 'node', node)

        pgbench.wait()
        pgbench.stdout.close()

        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type="ptrack", options=["--stream"])

        bbalance = node.execute(
            "postgres", "SELECT sum(bbalance) FROM pgbench_branches")
        delta = node.execute(
            "postgres", "SELECT sum(delta) FROM pgbench_history")

        self.assertEqual(bbalance, delta)

        node.stop()
        node.cleanup()
        # self.wrong_wal_clean(node, wal_segment_size)

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node, options=["-j", "4"]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))
        node.slow_start()
        bbalance = node.execute(
            "postgres", "SELECT sum(bbalance) FROM pgbench_branches")
        delta = node.execute(
            "postgres", "SELECT sum(delta) FROM pgbench_history")
        self.assertEqual(bbalance, delta)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_with_tablespace_mapping_1(self):
        """recovery using tablespace-mapping option"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # Create tablespace
        tblspc_path = os.path.join(node.base_dir, "tblspc")
        os.makedirs(tblspc_path)
        with node.connect("postgres") as con:
            con.connection.autocommit = True
            con.execute("CREATE TABLESPACE tblspc LOCATION '%s'" % tblspc_path)
            con.connection.autocommit = False
            con.execute("CREATE TABLE test (id int) TABLESPACE tblspc")
            con.execute("INSERT INTO test VALUES (1)")
            con.commit()

        backup_id = self.backup_node(backup_dir, 'node', node)
        self.assertEqual(self.show_pb(backup_dir, 'node')[0]['status'], "OK")

        # 1 - Try to restore to existing directory
        node.stop()
        try:
            self.restore_node(backup_dir, 'node', node)
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because restore destination is not empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Restore destination is not empty:',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # 2 - Try to restore to existing tablespace directory
        tblspc_path_tmp = os.path.join(node.base_dir, "tblspc_tmp")
        os.rename(tblspc_path, tblspc_path_tmp)
        node.cleanup()
        os.rename(tblspc_path_tmp, tblspc_path)
        try:
            self.restore_node(backup_dir, 'node', node)
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because restore tablespace destination is "
                "not empty.\n Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Restore tablespace destination is not empty:',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # 3 - Restore using tablespace-mapping to not empty directory
        tblspc_path_temp = os.path.join(node.base_dir, "tblspc_temp")
        os.mkdir(tblspc_path_temp)
        with open(os.path.join(tblspc_path_temp, 'file'), 'w+') as f:
            f.close()

        try:
            self.restore_node(
                backup_dir, 'node', node,
                options=["-T", "%s=%s" % (tblspc_path, tblspc_path_temp)])
            # we should die here because exception is what we expect to happen
            self.assertEqual(
                1, 0,
                "Expecting Error because restore tablespace destination is "
                "not empty.\n Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Restore tablespace destination is not empty:',
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # 4 - Restore using tablespace-mapping
        tblspc_path_new = os.path.join(node.base_dir, "tblspc_new")
        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-T", "%s=%s" % (tblspc_path, tblspc_path_new)]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        result = node.execute("postgres", "SELECT id FROM test")
        self.assertEqual(result[0][0], 1)

        # 4 - Restore using tablespace-mapping using page backup
        self.backup_node(backup_dir, 'node', node)
        with node.connect("postgres") as con:
            con.execute("INSERT INTO test VALUES (2)")
            con.commit()
        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type="page")

        show_pb = self.show_pb(backup_dir, 'node')
        self.assertEqual(show_pb[1]['status'], "OK")
        self.assertEqual(show_pb[2]['status'], "OK")

        node.stop()
        node.cleanup()
        tblspc_path_page = os.path.join(node.base_dir, "tblspc_page")

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-T", "%s=%s" % (tblspc_path_new, tblspc_path_page)]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        result = node.execute("postgres", "SELECT id FROM test OFFSET 1")
        self.assertEqual(result[0][0], 2)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_with_tablespace_mapping_2(self):
        """recovery using tablespace-mapping option and page backup"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # Full backup
        self.backup_node(backup_dir, 'node', node)
        self.assertEqual(self.show_pb(backup_dir, 'node')[0]['status'], "OK")

        # Create tablespace
        tblspc_path = os.path.join(node.base_dir, "tblspc")
        os.makedirs(tblspc_path)
        with node.connect("postgres") as con:
            con.connection.autocommit = True
            con.execute("CREATE TABLESPACE tblspc LOCATION '%s'" % tblspc_path)
            con.connection.autocommit = False
            con.execute(
                "CREATE TABLE tbl AS SELECT * "
                "FROM generate_series(0,3) AS integer")
            con.commit()

        # First page backup
        self.backup_node(backup_dir, 'node', node, backup_type="page")
        self.assertEqual(self.show_pb(backup_dir, 'node')[1]['status'], "OK")
        self.assertEqual(
            self.show_pb(backup_dir, 'node')[1]['backup-mode'], "PAGE")

        # Create tablespace table
        with node.connect("postgres") as con:
#            con.connection.autocommit = True
#            con.execute("CHECKPOINT")
#            con.connection.autocommit = False
            con.execute("CREATE TABLE tbl1 (a int) TABLESPACE tblspc")
            con.execute(
                "INSERT INTO tbl1 SELECT * "
                "FROM generate_series(0,3) AS integer")
            con.commit()

        # Second page backup
        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type="page")
        self.assertEqual(self.show_pb(backup_dir, 'node')[2]['status'], "OK")
        self.assertEqual(
            self.show_pb(backup_dir, 'node')[2]['backup-mode'], "PAGE")

        node.stop()
        node.cleanup()

        tblspc_path_new = os.path.join(node.base_dir, "tblspc_new")

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-T", "%s=%s" % (tblspc_path, tblspc_path_new)]),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))
        node.slow_start()

        count = node.execute("postgres", "SELECT count(*) FROM tbl")
        self.assertEqual(count[0][0], 4)
        count = node.execute("postgres", "SELECT count(*) FROM tbl1")
        self.assertEqual(count[0][0], 4)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_with_missing_or_corrupted_tablespace_map(self):
        """restore backup with missing or corrupted tablespace_map"""
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # Create tablespace
        self.create_tblspace_in_node(node, 'tblspace')
        node.pgbench_init(scale=1, tablespace='tblspace')

        # Full backup
        self.backup_node(backup_dir, 'node', node)

        # Change some data
        pgbench = node.pgbench(options=['-T', '10', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        # Page backup
        page_id = self.backup_node(backup_dir, 'node', node, backup_type="page")

        pgdata = self.pgdata_content(node.data_dir)

        node2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node2'))
        node2.cleanup()

        olddir = self.get_tblspace_path(node, 'tblspace')
        newdir = self.get_tblspace_path(node2, 'tblspace')

        # drop tablespace_map
        tablespace_map = os.path.join(
            backup_dir, 'backups', 'node',
            page_id, 'database', 'tablespace_map')

        tablespace_map_tmp = os.path.join(
            backup_dir, 'backups', 'node',
            page_id, 'database', 'tablespace_map_tmp')

        os.rename(tablespace_map, tablespace_map_tmp)

        try:
            self.restore_node(
                backup_dir, 'node', node2,
                options=["-T", "{0}={1}".format(olddir, newdir)])
            self.assertEqual(
                1, 0,
                "Expecting Error because tablespace_map is missing.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Tablespace map is missing: "{0}", '
                'probably backup {1} is corrupt, validate it'.format(
                    tablespace_map, page_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        try:
            self.restore_node(backup_dir, 'node', node2)
            self.assertEqual(
                1, 0,
                "Expecting Error because tablespace_map is missing.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Tablespace map is missing: "{0}", '
                'probably backup {1} is corrupt, validate it'.format(
                    tablespace_map, page_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        copyfile(tablespace_map_tmp, tablespace_map)

        with open(tablespace_map, "a") as f:
            f.write("HELLO\n")

        try:
            self.restore_node(
                backup_dir, 'node', node2,
                options=["-T", "{0}={1}".format(olddir, newdir)])
            self.assertEqual(
                1, 0,
                "Expecting Error because tablespace_map is corupted.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Invalid CRC of tablespace map file "{0}"'.format(tablespace_map),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        try:
            self.restore_node(backup_dir, 'node', node2)
            self.assertEqual(
                1, 0,
                "Expecting Error because tablespace_map is corupted.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Invalid CRC of tablespace map file "{0}"'.format(tablespace_map),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # rename it back
        os.rename(tablespace_map_tmp, tablespace_map)

        print(self.restore_node(
            backup_dir, 'node', node2,
            options=["-T", "{0}={1}".format(olddir, newdir)]))

        pgdata_restored = self.pgdata_content(node2.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_archive_node_backup_stream_restore_to_recovery_time(self):
        """
        make node with archiving, make stream backup,
        make PITR to Recovery Time
        """
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

        backup_id = self.backup_node(
            backup_dir, 'node', node, options=["--stream"])
        node.safe_psql("postgres", "create table t_heap(a int)")

        node.stop()
        node.cleanup()

        recovery_time = self.show_pb(
            backup_dir, 'node', backup_id)['recovery-time']

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--time={0}'.format(recovery_time),
                    "--recovery-target-action=promote"
                    ]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        result = node.psql("postgres", 'select * from t_heap')
        self.assertTrue('does not exist' in result[2].decode("utf-8"))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_archive_node_backup_stream_restore_to_recovery_time(self):
        """
        make node with archiving, make stream backup,
        make PITR to Recovery Time
        """
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

        backup_id = self.backup_node(
            backup_dir, 'node', node, options=["--stream"])
        node.safe_psql("postgres", "create table t_heap(a int)")
        node.stop()
        node.cleanup()

        recovery_time = self.show_pb(
            backup_dir, 'node', backup_id)['recovery-time']

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--time={0}'.format(recovery_time),
                    "--recovery-target-action=promote"
                ]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()
        result = node.psql("postgres", 'select * from t_heap')
        self.assertTrue('does not exist' in result[2].decode("utf-8"))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_archive_node_backup_stream_pitr(self):
        """
        make node with archiving, make stream backup,
        create table t_heap, make pitr to Recovery Time,
        check that t_heap do not exists
        """
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

        backup_id = self.backup_node(
            backup_dir, 'node', node, options=["--stream"])
        node.safe_psql("postgres", "create table t_heap(a int)")
        node.cleanup()

        recovery_time = self.show_pb(
            backup_dir, 'node', backup_id)['recovery-time']

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    "-j", "4", '--time={0}'.format(recovery_time),
                    "--recovery-target-action=promote"
                    ]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        node.slow_start()

        result = node.psql("postgres", 'select * from t_heap')
        self.assertEqual(True, 'does not exist' in result[2].decode("utf-8"))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_archive_node_backup_archive_pitr_2(self):
        """
        make node with archiving, make archive backup,
        create table t_heap, make pitr to Recovery Time,
        check that t_heap do not exists
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

        backup_id = self.backup_node(backup_dir, 'node', node)
        if self.paranoia:
            pgdata = self.pgdata_content(node.data_dir)

        node.safe_psql("postgres", "create table t_heap(a int)")
        node.stop()

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        recovery_time = self.show_pb(
            backup_dir, 'node', backup_id)['recovery-time']

        self.assertIn(
            "INFO: Restore of backup {0} completed.".format(backup_id),
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=[
                    "-j", "4", '--time={0}'.format(recovery_time),
                    "--recovery-target-action=promote"]
            ),
            '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                repr(self.output), self.cmd))

        if self.paranoia:
            pgdata_restored = self.pgdata_content(node_restored.data_dir)
            self.compare_pgdata(pgdata, pgdata_restored)

        self.set_auto_conf(node_restored, {'port': node_restored.port})

        node_restored.slow_start()

        result = node_restored.psql("postgres", 'select * from t_heap')
        self.assertTrue('does not exist' in result[2].decode("utf-8"))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_archive_restore_to_restore_point(self):
        """
        make node with archiving, make archive backup,
        create table t_heap, make pitr to Recovery Time,
        check that t_heap do not exists
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

        node.safe_psql(
            "postgres",
            "create table t_heap as select generate_series(0,10000)")
        result = node.safe_psql(
            "postgres",
            "select * from t_heap")
        node.safe_psql(
            "postgres", "select pg_create_restore_point('savepoint')")
        node.safe_psql(
            "postgres",
            "create table t_heap_1 as select generate_series(0,10000)")
        node.cleanup()

        self.restore_node(
            backup_dir, 'node', node,
            options=[
                "--recovery-target-name=savepoint",
                "--recovery-target-action=promote"])

        node.slow_start()

        result_new = node.safe_psql("postgres", "select * from t_heap")
        res = node.psql("postgres", "select * from t_heap_1")
        self.assertEqual(
            res[0], 1,
            "Table t_heap_1 should not exist in restored instance")

        self.assertEqual(result, result_new)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_zags_block_corrupt(self):
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

        conn = node.connect()
        with node.connect("postgres") as conn:

            conn.execute(
                "create table tbl(i int)")
            conn.commit()
            conn.execute(
                "create index idx ON tbl (i)")
            conn.commit()
            conn.execute(
                "insert into tbl select i from generate_series(0,400) as i")
            conn.commit()
            conn.execute(
                "select pg_relation_size('idx')")
            conn.commit()
            conn.execute(
                "delete from tbl where i < 100")
            conn.commit()
            conn.execute(
                "explain analyze select i from tbl order by i")
            conn.commit()
            conn.execute(
                "select i from tbl order by i")
            conn.commit()
            conn.execute(
                "create extension pageinspect")
            conn.commit()
            print(conn.execute(
                "select * from bt_page_stats('idx',1)"))
            conn.commit()
            conn.execute(
                "insert into tbl select i from generate_series(0,100) as i")
            conn.commit()
            conn.execute(
                "insert into tbl select i from generate_series(0,100) as i")
            conn.commit()
            conn.execute(
                "insert into tbl select i from generate_series(0,100) as i")
            conn.commit()
            conn.execute(
                "insert into tbl select i from generate_series(0,100) as i")

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'),
            initdb_params=['--data-checksums'])

        node_restored.cleanup()

        self.restore_node(
            backup_dir, 'node', node_restored)

        self.set_auto_conf(
            node_restored,
            {'archive_mode': 'off', 'hot_standby': 'on', 'port': node_restored.port})

        node_restored.slow_start()

    @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_zags_block_corrupt_1(self):
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'],
            pg_options={
                'full_page_writes': 'on'}
            )
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        self.backup_node(backup_dir, 'node', node)

        node.safe_psql('postgres', 'create table tbl(i int)')

        node.safe_psql('postgres', 'create index idx ON tbl (i)')

        node.safe_psql(
            'postgres',
            'insert into tbl select i from generate_series(0,100000) as i')

        node.safe_psql(
            'postgres',
            'delete from tbl where i%2 = 0')

        node.safe_psql(
            'postgres',
            'explain analyze select i from tbl order by i')

        node.safe_psql(
            'postgres',
            'select i from tbl order by i')

        node.safe_psql(
            'postgres',
            'create extension pageinspect')

        node.safe_psql(
            'postgres',
            'insert into tbl select i from generate_series(0,100) as i')

        node.safe_psql(
            'postgres',
            'insert into tbl select i from generate_series(0,100) as i')

        node.safe_psql(
            'postgres',
            'insert into tbl select i from generate_series(0,100) as i')

        node.safe_psql(
            'postgres',
            'insert into tbl select i from generate_series(0,100) as i')

        self.switch_wal_segment(node)

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'),
            initdb_params=['--data-checksums'])

        pgdata = self.pgdata_content(node.data_dir)

        node_restored.cleanup()

        self.restore_node(
            backup_dir, 'node', node_restored)

        self.set_auto_conf(
            node_restored,
            {'archive_mode': 'off', 'hot_standby': 'on', 'port': node_restored.port})

        node_restored.slow_start()

        while True:
            with open(node_restored.pg_log_file, 'r') as f:
                if 'selected new timeline ID' in f.read():
                    break

        # with open(node_restored.pg_log_file, 'r') as f:
        #        print(f.read())

        pgdata_restored = self.pgdata_content(node_restored.data_dir)

        self.compare_pgdata(pgdata, pgdata_restored)

#        pg_xlogdump_path = self.get_bin_path('pg_xlogdump')

#        pg_xlogdump = self.run_binary(
#            [
#                pg_xlogdump_path, '-b',
#                os.path.join(backup_dir, 'wal', 'node', '000000010000000000000003'),
#                ' | ', 'grep', 'Btree', ''
#            ], async=False)

        if pg_xlogdump.returncode:
            self.assertFalse(
                True,
                'Failed to start pg_wal_dump: {0}'.format(
                    pg_receivexlog.communicate()[1]))

    # @unittest.skip("skip")
    def test_restore_chain(self):
        """
        make node, take full backup, take several
        ERROR delta backups, take valid delta backup,
        restore must be successfull
        """
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

        # Take FULL
        self.backup_node(
            backup_dir, 'node', node)

        # Take DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='delta', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='delta', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        # Take DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='delta', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[0]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[1]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[2]['status'],
            'Backup STATUS should be "ERROR"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[3]['status'],
            'Backup STATUS should be "ERROR"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[4]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[5]['status'],
            'Backup STATUS should be "ERROR"')

        node.cleanup()

        self.restore_node(backup_dir, 'node', node)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_chain_with_corrupted_backup(self):
        """more complex test_restore_chain()"""
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

        # Take FULL
        self.backup_node(
            backup_dir, 'node', node)

        # Take DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='page')

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='page', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        # Take 1 DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='delta', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        # Take 2 DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # Take ERROR DELTA
        try:
            self.backup_node(
                backup_dir, 'node', node,
                backup_type='delta', options=['-U', 'wrong_name'])
        except ProbackupException as e:
            pass

        # Take 3 DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # Corrupted 4 DELTA
        corrupt_id = self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # ORPHAN 5 DELTA
        restore_target_id = self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # ORPHAN 6 DELTA
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # NEXT FULL BACKUP
        self.backup_node(
            backup_dir, 'node', node, backup_type='full')

        # Next Delta
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta')

        # do corrupt 6 DELTA backup
        file = os.path.join(
            backup_dir, 'backups', 'node',
            corrupt_id, 'database', 'global', 'pg_control')

        file_new = os.path.join(backup_dir, 'pg_control')
        os.rename(file, file_new)

        # RESTORE BACKUP
        node.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node', node, backup_id=restore_target_id)
            self.assertEqual(
                1, 0,
                "Expecting Error because restore backup is corrupted.\n "
                "Output: {0} \n CMD: {1}".format(
                    repr(self.output), self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: Backup {0} is orphan'.format(restore_target_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[0]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[1]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[2]['status'],
            'Backup STATUS should be "ERROR"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[3]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[4]['status'],
            'Backup STATUS should be "ERROR"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[5]['status'],
            'Backup STATUS should be "OK"')

        self.assertEqual(
            'ERROR',
            self.show_pb(backup_dir, 'node')[6]['status'],
            'Backup STATUS should be "ERROR"')

        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[7]['status'],
            'Backup STATUS should be "OK"')

        # corruption victim
        self.assertEqual(
            'CORRUPT',
            self.show_pb(backup_dir, 'node')[8]['status'],
            'Backup STATUS should be "CORRUPT"')

        # orphaned child
        self.assertEqual(
            'ORPHAN',
            self.show_pb(backup_dir, 'node')[9]['status'],
            'Backup STATUS should be "ORPHAN"')

        # orphaned child
        self.assertEqual(
            'ORPHAN',
            self.show_pb(backup_dir, 'node')[10]['status'],
            'Backup STATUS should be "ORPHAN"')

        # next FULL
        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[11]['status'],
            'Backup STATUS should be "OK"')

        # next DELTA
        self.assertEqual(
            'OK',
            self.show_pb(backup_dir, 'node')[12]['status'],
            'Backup STATUS should be "OK"')

        node.cleanup()

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_backup_from_future(self):
        """more complex test_restore_chain()"""
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

        # Take FULL
        self.backup_node(backup_dir, 'node', node)

        node.pgbench_init(scale=5)
        # pgbench = node.pgbench(options=['-T', '20', '-c', '2'])
        # pgbench.wait()

        # Take PAGE from future
        backup_id = self.backup_node(
            backup_dir, 'node', node, backup_type='page')

        with open(
                os.path.join(
                    backup_dir, 'backups', 'node',
                    backup_id, "backup.control"), "a") as conf:
            conf.write("start-time='{:%Y-%m-%d %H:%M:%S}'\n".format(
                datetime.now() + timedelta(days=3)))

        # rename directory
        new_id = self.show_pb(backup_dir, 'node')[1]['id']

        os.rename(
            os.path.join(backup_dir, 'backups', 'node', backup_id),
            os.path.join(backup_dir, 'backups', 'node', new_id))

        pgbench = node.pgbench(options=['-T', '7', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        backup_id = self.backup_node(backup_dir, 'node', node, backup_type='page')
        pgdata = self.pgdata_content(node.data_dir)

        node.cleanup()
        self.restore_node(backup_dir, 'node', node, backup_id=backup_id)

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_target_immediate_stream(self):
        """
        correct handling of immediate recovery target
        for STREAM backups
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

        # Take FULL
        self.backup_node(
            backup_dir, 'node', node, options=['--stream'])

        # Take delta
        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta', options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')

        # restore delta backup
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node, options=['--immediate'])

        self.assertTrue(
            os.path.isfile(recovery_conf),
            "File {0} do not exists".format(recovery_conf))

        # restore delta backup
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node, options=['--recovery-target=immediate'])

        self.assertTrue(
            os.path.isfile(recovery_conf),
            "File {0} do not exists".format(recovery_conf))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_target_immediate_archive(self):
        """
        correct handling of immediate recovery target
        for ARCHIVE backups
        """
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

        # Take FULL
        self.backup_node(
            backup_dir, 'node', node)

        # Take delta
        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta')

        pgdata = self.pgdata_content(node.data_dir)

        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')

        # restore page backup
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node, options=['--immediate'])

        # For archive backup with immediate recovery target
        # recovery.conf is mandatory
        with open(recovery_conf, 'r') as f:
            self.assertIn("recovery_target = 'immediate'", f.read())

        # restore page backup
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node, options=['--recovery-target=immediate'])

        # For archive backup with immediate recovery target
        # recovery.conf is mandatory
        with open(recovery_conf, 'r') as f:
            self.assertIn("recovery_target = 'immediate'", f.read())

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_target_latest_archive(self):
        """
        make sure that recovery_target 'latest'
        is default recovery target
        """
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

        # Take FULL
        self.backup_node(backup_dir, 'node', node)

        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')

        # restore
        node.cleanup()
        self.restore_node(backup_dir, 'node', node)

        # hash_1 = hashlib.md5(
        #     open(recovery_conf, 'rb').read()).hexdigest()

        with open(recovery_conf, 'r') as f:
            content_1 = ''
            while True:
                line = f.readline()

                if not line:
                    break
                if line.startswith("#"):
                    continue
                content_1 += line

        node.cleanup()
        self.restore_node(backup_dir, 'node', node, options=['--recovery-target=latest'])

        # hash_2 = hashlib.md5(
        #     open(recovery_conf, 'rb').read()).hexdigest()

        with open(recovery_conf, 'r') as f:
            content_2 = ''
            while True:
                line = f.readline()

                if not line:
                    break
                if line.startswith("#"):
                    continue
                content_2 += line

        self.assertEqual(content_1, content_2)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_target_new_options(self):
        """
        check that new --recovery-target-*
        options are working correctly
        """
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

        # Take FULL
        self.backup_node(backup_dir, 'node', node)

        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')

        node.pgbench_init(scale=2)
        pgbench = node.pgbench(
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pgbench.wait()
        pgbench.stdout.close()

        node.safe_psql(
            "postgres",
            "CREATE TABLE tbl0005 (a text)")

        node.safe_psql(
            "postgres", "select pg_create_restore_point('savepoint')")

        target_name = 'savepoint'

        target_time = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
        with node.connect("postgres") as con:
            res = con.execute(
                "INSERT INTO tbl0005 VALUES ('inserted') RETURNING (xmin)")
            con.commit()
            target_xid = res[0][0]

        with node.connect("postgres") as con:
            con.execute("INSERT INTO tbl0005 VALUES (1)")
            con.commit()
            if self.get_version(node) > self.version_to_num('10.0'):
                res = con.execute("SELECT pg_current_wal_lsn()")
            else:
                res = con.execute("SELECT pg_current_xlog_location()")

            con.commit()
            con.execute("INSERT INTO tbl0005 VALUES (2)")
            con.commit()
            xlogid, xrecoff = res[0][0].split('/')
            xrecoff = hex(int(xrecoff, 16) + 1)[2:]
            target_lsn = "{0}/{1}".format(xlogid, xrecoff)

        # Restore with recovery target time
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node,
            options=[
                '--recovery-target-time={0}'.format(target_time),
                "--recovery-target-action=promote",
                '--recovery-target-timeline=1',
                ])

        with open(recovery_conf, 'r') as f:
            recovery_conf_content = f.read()

        self.assertIn(
            "recovery_target_time = '{0}'".format(target_time),
            recovery_conf_content)

        self.assertIn(
            "recovery_target_action = 'promote'",
            recovery_conf_content)

        self.assertIn(
            "recovery_target_timeline = '1'",
            recovery_conf_content)

        node.slow_start()

        # Restore with recovery target xid
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node,
            options=[
                '--recovery-target-xid={0}'.format(target_xid),
                "--recovery-target-action=promote",
                '--recovery-target-timeline=1',
                ])

        with open(recovery_conf, 'r') as f:
            recovery_conf_content = f.read()

        self.assertIn(
            "recovery_target_xid = '{0}'".format(target_xid),
            recovery_conf_content)

        self.assertIn(
            "recovery_target_action = 'promote'",
            recovery_conf_content)

        self.assertIn(
            "recovery_target_timeline = '1'",
            recovery_conf_content)

        node.slow_start()

        # Restore with recovery target name
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node,
            options=[
                '--recovery-target-name={0}'.format(target_name),
                "--recovery-target-action=promote",
                '--recovery-target-timeline=1',
                ])

        with open(recovery_conf, 'r') as f:
            recovery_conf_content = f.read()

        self.assertIn(
            "recovery_target_name = '{0}'".format(target_name),
            recovery_conf_content)

        self.assertIn(
            "recovery_target_action = 'promote'",
            recovery_conf_content)

        self.assertIn(
            "recovery_target_timeline = '1'",
            recovery_conf_content)

        node.slow_start()

        # Restore with recovery target lsn
        if self.get_version(node) >= 100000:

            node.cleanup()
            self.restore_node(
                backup_dir, 'node', node,
                options=[
                    '--recovery-target-lsn={0}'.format(target_lsn),
                    "--recovery-target-action=promote",
                    '--recovery-target-timeline=1',
                    ])

            with open(recovery_conf, 'r') as f:
                recovery_conf_content = f.read()

            self.assertIn(
                "recovery_target_lsn = '{0}'".format(target_lsn),
                recovery_conf_content)

            self.assertIn(
                "recovery_target_action = 'promote'",
                recovery_conf_content)

            self.assertIn(
                "recovery_target_timeline = '1'",
                recovery_conf_content)

            node.slow_start()

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_smart_restore(self):
        """
        make node, create database, take full backup, drop database,
        take incremental backup and restore it,
        make sure that files from dropped database are not
        copied during restore
        https://github.com/postgrespro/pg_probackup/issues/63
        """
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

        # create database
        node.safe_psql(
            "postgres",
            "CREATE DATABASE testdb")

        # take FULL backup
        full_id = self.backup_node(backup_dir, 'node', node)

        # drop database
        node.safe_psql(
            "postgres",
            "DROP DATABASE testdb")

        # take PAGE backup
        page_id = self.backup_node(
            backup_dir, 'node', node, backup_type='page')

        # restore PAGE backup
        node.cleanup()
        self.restore_node(
            backup_dir, 'node', node, backup_id=page_id,
            options=['--no-validate', '--log-level-file=VERBOSE'])

        logfile = os.path.join(backup_dir, 'log', 'pg_probackup.log')
        with open(logfile, 'r') as f:
                logfile_content = f.read()

        # get delta between FULL and PAGE filelists
        filelist_full = self.get_backup_filelist(
            backup_dir, 'node', full_id)

        filelist_page = self.get_backup_filelist(
            backup_dir, 'node', page_id)

        filelist_diff = self.get_backup_filelist_diff(
            filelist_full, filelist_page)

        for file in filelist_diff:
            self.assertNotIn(file, logfile_content)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_pg_11_group_access(self):
        """
        test group access for PG >= 11
        """
        if self.pg_config_version < self.version_to_num('11.0'):
            return unittest.skip('You need PostgreSQL >= 11 for this test')

        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=[
                '--data-checksums',
                '--allow-group-access'])

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        # take FULL backup
        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        # restore backup
        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        self.restore_node(
            backup_dir, 'node', node_restored)

        # compare pgdata permissions
        pgdata_restored = self.pgdata_content(node_restored.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_concurrent_drop_table(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=1)

        # FULL backup
        self.backup_node(
            backup_dir, 'node', node,
            options=['--stream', '--compress'])

        # DELTA backup
        gdb = self.backup_node(
            backup_dir, 'node', node, backup_type='delta',
            options=['--stream', '--compress', '--no-validate'],
            gdb=True)

        gdb.set_breakpoint('backup_data_file')
        gdb.run_until_break()

        node.safe_psql(
            'postgres',
            'DROP TABLE pgbench_accounts')

        # do checkpoint to guarantee filenode removal
        node.safe_psql(
            'postgres',
            'CHECKPOINT')

        gdb.remove_all_breakpoints()
        gdb.continue_execution_until_exit()

        pgdata = self.pgdata_content(node.data_dir)
        node.cleanup()

        self.restore_node(
            backup_dir, 'node', node, options=['--no-validate'])

        pgdata_restored = self.pgdata_content(node.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_lost_non_data_file(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        backup_id = self.backup_node(
            backup_dir, 'node', node, options=['--stream'])

        file = os.path.join(
            backup_dir, 'backups', 'node',
            backup_id, 'database', 'postgresql.auto.conf')

        os.remove(file)

        node.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node', node, options=['--no-validate'])
            self.assertEqual(
                1, 0,
                "Expecting Error because of non-data file dissapearance.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'No such file or directory', e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))
            self.assertIn(
                'ERROR: Backup files restoring failed', e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_partial_restore_exclude(self):
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

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)
        pgdata = self.pgdata_content(node.data_dir)

        # restore FULL backup
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node',
                node_restored_1, options=[
                    "--db-include=db1",
                    "--db-exclude=db2"])
            self.assertEqual(
                1, 0,
                "Expecting Error because of 'db-exclude' and 'db-include'.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: You cannot specify '--db-include' "
                "and '--db-exclude' together", e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(
            backup_dir, 'node', node_restored_1)

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored_1)

        db1_path = os.path.join(
            node_restored_1.data_dir, 'base', db_list['db1'])
        db5_path = os.path.join(
            node_restored_1.data_dir, 'base', db_list['db5'])

        self.truncate_every_file_in_dir(db1_path)
        self.truncate_every_file_in_dir(db5_path)
        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)

        node_restored_2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_2'))
        node_restored_2.cleanup()

        self.restore_node(
            backup_dir, 'node',
            node_restored_2, options=[
                "--db-exclude=db1",
                "--db-exclude=db5"])

        pgdata_restored_2 = self.pgdata_content(node_restored_2.data_dir)
        self.compare_pgdata(pgdata_restored_1, pgdata_restored_2)

        self.set_auto_conf(node_restored_2, {'port': node_restored_2.port})

        node_restored_2.slow_start()

        node_restored_2.safe_psql(
            'postgres',
            'select 1')

        try:
            node_restored_2.safe_psql(
                'db1',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node_restored_2.safe_psql(
                'db5',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node_restored_2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_partial_restore_exclude_tablespace(self):
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

        cat_version = node.get_control_data()["Catalog version number"]
        version_specific_dir = 'PG_' + node.major_version_str + '_' + cat_version

        # PG_10_201707211
        # pg_tblspc/33172/PG_9.5_201510051/16386/

        self.create_tblspace_in_node(node, 'somedata')

        node_tablespace = self.get_tblspace_path(node, 'somedata')

        tbl_oid = node.safe_psql(
            'postgres',
            "SELECT oid "
            "FROM pg_tablespace "
            "WHERE spcname = 'somedata'").decode('utf-8').rstrip()

        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0} tablespace somedata'.format(i))

        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').decode('utf-8').rstrip()

        db_list_splitted = db_list_raw.splitlines()

        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)
        pgdata = self.pgdata_content(node.data_dir)

        # restore FULL backup
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        node1_tablespace = self.get_tblspace_path(node_restored_1, 'somedata')

        self.restore_node(
            backup_dir, 'node',
            node_restored_1, options=[
                "-T", "{0}={1}".format(
                    node_tablespace, node1_tablespace)])

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored_1)

        # truncate every db
        for db in db_list:
            # with exception below
            if db in ['db1', 'db5']:
                self.truncate_every_file_in_dir(
                    os.path.join(
                        node_restored_1.data_dir, 'pg_tblspc',
                        tbl_oid, version_specific_dir, db_list[db]))

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)

        node_restored_2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_2'))
        node_restored_2.cleanup()
        node2_tablespace = self.get_tblspace_path(node_restored_2, 'somedata')

        self.restore_node(
            backup_dir, 'node',
            node_restored_2, options=[
                "--db-exclude=db1",
                "--db-exclude=db5",
                "-T", "{0}={1}".format(
                    node_tablespace, node2_tablespace)])

        pgdata_restored_2 = self.pgdata_content(node_restored_2.data_dir)
        self.compare_pgdata(pgdata_restored_1, pgdata_restored_2)

        self.set_auto_conf(node_restored_2, {'port': node_restored_2.port})

        node_restored_2.slow_start()

        node_restored_2.safe_psql(
            'postgres',
            'select 1')

        try:
            node_restored_2.safe_psql(
                'db1',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node_restored_2.safe_psql(
                'db5',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node_restored_2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_partial_restore_include(self):
        """
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

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)
        pgdata = self.pgdata_content(node.data_dir)

        # restore FULL backup
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node',
                node_restored_1, options=[
                    "--db-include=db1",
                    "--db-exclude=db2"])
            self.assertEqual(
                1, 0,
                "Expecting Error because of 'db-exclude' and 'db-include'.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: You cannot specify '--db-include' "
                "and '--db-exclude' together", e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(
            backup_dir, 'node', node_restored_1)

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored_1)

        # truncate every db
        for db in db_list:
            # with exception below
            if db in ['template0', 'template1', 'postgres', 'db1', 'db5']:
                continue
            self.truncate_every_file_in_dir(
                os.path.join(
                    node_restored_1.data_dir, 'base', db_list[db]))

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)

        node_restored_2 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_2'))
        node_restored_2.cleanup()

        self.restore_node(
            backup_dir, 'node',
            node_restored_2, options=[
                "--db-include=db1",
                "--db-include=db5",
                "--db-include=postgres"])

        pgdata_restored_2 = self.pgdata_content(node_restored_2.data_dir)
        self.compare_pgdata(pgdata_restored_1, pgdata_restored_2)

        self.set_auto_conf(node_restored_2, {'port': node_restored_2.port})
        node_restored_2.slow_start()

        node_restored_2.safe_psql(
            'db1',
            'select 1')

        node_restored_2.safe_psql(
            'db5',
            'select 1')

        node_restored_2.safe_psql(
            'template1',
            'select 1')

        try:
            node_restored_2.safe_psql(
                'db2',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        try:
            node_restored_2.safe_psql(
                'db10',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        with open(node_restored_2.pg_log_file, 'r') as f:
            output = f.read()

        self.assertNotIn('PANIC', output)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_partial_restore_backward_compatibility_1(self):
        """
        old binary should be of version < 2.2.0
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir, old_binary=True)
        self.add_instance(backup_dir, 'node', node, old_binary=True)

        node.slow_start()

        # create databases
        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        # FULL backup with old binary, without partial restore support
        backup_id = self.backup_node(
            backup_dir, 'node', node,
            old_binary=True, options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node',
                node_restored, options=[
                    "--db-exclude=db5"])
            self.assertEqual(
                1, 0,
                "Expecting Error because backup do not support partial restore.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} doesn't contain a database_map, "
                "partial restore is impossible".format(backup_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(backup_dir, 'node', node_restored)

        pgdata_restored = self.pgdata_content(node_restored.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # incremental backup with partial restore support
        for i in range(11, 15, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        # get db list
        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').rstrip()
        db_list_splitted = db_list_raw.splitlines()
        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta', options=['--stream'])

        # get etalon
        node_restored.cleanup()
        self.restore_node(backup_dir, 'node', node_restored)
        self.truncate_every_file_in_dir(
                os.path.join(
                    node_restored.data_dir, 'base', db_list['db5']))
        self.truncate_every_file_in_dir(
                os.path.join(
                    node_restored.data_dir, 'base', db_list['db14']))
        pgdata_restored = self.pgdata_content(node_restored.data_dir)

        # get new node
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        self.restore_node(
                backup_dir, 'node',
                node_restored_1, options=[
                    "--db-exclude=db5",
                    "--db-exclude=db14"])

        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)

        self.compare_pgdata(pgdata_restored, pgdata_restored_1)

    def test_partial_restore_backward_compatibility_merge(self):
        """
        old binary should be of version < 2.2.0
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir, old_binary=True)
        self.add_instance(backup_dir, 'node', node, old_binary=True)

        node.slow_start()

        # create databases
        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        # FULL backup with old binary, without partial restore support
        backup_id = self.backup_node(
            backup_dir, 'node', node,
            old_binary=True, options=['--stream'])

        pgdata = self.pgdata_content(node.data_dir)

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node',
                node_restored, options=[
                    "--db-exclude=db5"])
            self.assertEqual(
                1, 0,
                "Expecting Error because backup do not support partial restore.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} doesn't contain a database_map, "
                "partial restore is impossible.".format(backup_id),
                e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        self.restore_node(backup_dir, 'node', node_restored)

        pgdata_restored = self.pgdata_content(node_restored.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # incremental backup with partial restore support
        for i in range(11, 15, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        # get db list
        db_list_raw = node.safe_psql(
            'postgres',
            'SELECT to_json(a) '
            'FROM (SELECT oid, datname FROM pg_database) a').rstrip()
        db_list_splitted = db_list_raw.splitlines()
        db_list = {}
        for line in db_list_splitted:
            line = json.loads(line)
            db_list[line['datname']] = line['oid']

        backup_id = self.backup_node(
            backup_dir, 'node', node,
            backup_type='delta', options=['--stream'])

        # get etalon
        node_restored.cleanup()
        self.restore_node(backup_dir, 'node', node_restored)
        self.truncate_every_file_in_dir(
                os.path.join(
                    node_restored.data_dir, 'base', db_list['db5']))
        self.truncate_every_file_in_dir(
                os.path.join(
                    node_restored.data_dir, 'base', db_list['db14']))
        pgdata_restored = self.pgdata_content(node_restored.data_dir)

        # get new node
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        # merge
        self.merge_backup(backup_dir, 'node', backup_id=backup_id)

        self.restore_node(
                backup_dir, 'node',
                node_restored_1, options=[
                    "--db-exclude=db5",
                    "--db-exclude=db14"])
        pgdata_restored_1 = self.pgdata_content(node_restored_1.data_dir)

        self.compare_pgdata(pgdata_restored, pgdata_restored_1)

    def test_empty_and_mangled_database_map(self):
        """
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)

        node.slow_start()

        # create databases
        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        # FULL backup with database_map
        backup_id = self.backup_node(
            backup_dir, 'node', node, options=['--stream'])
        pgdata = self.pgdata_content(node.data_dir)

        # truncate database_map
        path = os.path.join(
            backup_dir, 'backups', 'node',
            backup_id, 'database', 'database_map')
        with open(path, "w") as f:
            f.close()

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-include=db1", '--no-validate'])
            self.assertEqual(
                1, 0,
                "Expecting Error because database_map is empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} has empty or mangled database_map, "
                "partial restore is impossible".format(backup_id), e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-exclude=db1", '--no-validate'])
            self.assertEqual(
                1, 0,
                "Expecting Error because database_map is empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} has empty or mangled database_map, "
                "partial restore is impossible".format(backup_id), e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # mangle database_map
        with open(path, "w") as f:
            f.write("42")
            f.close()

        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-include=db1", '--no-validate'])
            self.assertEqual(
                1, 0,
                "Expecting Error because database_map is empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: field "dbOid" is not found in the line 42 of '
                'the file backup_content.control', e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-exclude=db1", '--no-validate'])
            self.assertEqual(
                1, 0,
                "Expecting Error because database_map is empty.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                'ERROR: field "dbOid" is not found in the line 42 of '
                'the file backup_content.control', e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # check that simple restore is still possible
        self.restore_node(
            backup_dir, 'node', node_restored, options=['--no-validate'])

        pgdata_restored = self.pgdata_content(node_restored.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

    def test_missing_database_map(self):
        """
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            ptrack_enable=self.ptrack,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)

        node.slow_start()

        # create databases
        for i in range(1, 10, 1):
            node.safe_psql(
                'postgres',
                'CREATE database db{0}'.format(i))

        node.safe_psql(
            "postgres",
            "CREATE DATABASE backupdb")

        # PG 9.5
        if self.get_version(node) < 90600:
            node.safe_psql(
                'backupdb',
                "REVOKE ALL ON DATABASE backupdb from PUBLIC; "
                "REVOKE ALL ON SCHEMA public from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA pg_catalog from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA information_schema from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA information_schema FROM PUBLIC; "
                "CREATE ROLE backup WITH LOGIN REPLICATION; "
                "GRANT CONNECT ON DATABASE backupdb to backup; "
                "GRANT USAGE ON SCHEMA pg_catalog TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_proc TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_extension TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_database TO backup; " # for partial restore, checkdb and ptrack
                "GRANT EXECUTE ON FUNCTION pg_catalog.nameeq(name, name) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.textout(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.timestamptz(timestamp with time zone, integer) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO backup;"
            )
        # PG 9.6
        elif self.get_version(node) > 90600 and self.get_version(node) < 100000:
            node.safe_psql(
                'backupdb',
                "REVOKE ALL ON DATABASE backupdb from PUBLIC; "
                "REVOKE ALL ON SCHEMA public from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA pg_catalog from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA information_schema from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA information_schema FROM PUBLIC; "
                "CREATE ROLE backup WITH LOGIN REPLICATION; "
                "GRANT CONNECT ON DATABASE backupdb to backup; "
                "GRANT USAGE ON SCHEMA pg_catalog TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_proc TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_extension TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_database TO backup; " # for partial restore, checkdb and ptrack
                "GRANT EXECUTE ON FUNCTION pg_catalog.nameeq(name, name) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.textout(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.timestamptz(timestamp with time zone, integer) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_control_system() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean, boolean) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup(boolean) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_create_restore_point(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_switch_xlog() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_last_xlog_replay_location() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO backup;"
            )
        # >= 10
        else:
            node.safe_psql(
                'backupdb',
                "REVOKE ALL ON DATABASE backupdb from PUBLIC; "
                "REVOKE ALL ON SCHEMA public from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA pg_catalog from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA pg_catalog FROM PUBLIC; "
                "REVOKE ALL ON SCHEMA information_schema from PUBLIC; "
                "REVOKE ALL ON ALL TABLES IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL FUNCTIONS IN SCHEMA information_schema FROM PUBLIC; "
                "REVOKE ALL ON ALL SEQUENCES IN SCHEMA information_schema FROM PUBLIC; "
                "CREATE ROLE backup WITH LOGIN REPLICATION; "
                "GRANT CONNECT ON DATABASE backupdb to backup; "
                "GRANT USAGE ON SCHEMA pg_catalog TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_proc TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_extension TO backup; "
                "GRANT SELECT ON TABLE pg_catalog.pg_database TO backup; " # for partial restore, checkdb and ptrack
                "GRANT EXECUTE ON FUNCTION pg_catalog.nameeq(name, name) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.current_setting(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_is_in_recovery() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_control_system() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_start_backup(text, boolean, boolean) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_stop_backup(boolean, boolean) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_create_restore_point(text) TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_switch_wal() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.pg_last_wal_replay_lsn() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_current_snapshot() TO backup; "
                "GRANT EXECUTE ON FUNCTION pg_catalog.txid_snapshot_xmax(txid_snapshot) TO backup;"
            )

        if self.ptrack:
            # TODO why backup works without these grants ?
            #    'pg_ptrack_get_pagemapset(pg_lsn)',
            #    'pg_ptrack_control_lsn()',
            # because PUBLIC
            node.safe_psql(
                "backupdb",
                "CREATE SCHEMA ptrack; "
                "GRANT USAGE ON SCHEMA ptrack TO backup; "
                "CREATE EXTENSION ptrack WITH SCHEMA ptrack")

        if ProbackupTest.enterprise:
            node.safe_psql(
                "backupdb",
                "GRANT EXECUTE ON FUNCTION pg_catalog.pgpro_edition() TO backup")

            node.safe_psql(
                "backupdb",
                "GRANT EXECUTE ON FUNCTION pg_catalog.pgpro_version() TO backup")

        # FULL backup without database_map
        backup_id = self.backup_node(
            backup_dir, 'node', node, datname='backupdb',
            options=['--stream', "-U", "backup", '--log-level-file=verbose'])

        pgdata = self.pgdata_content(node.data_dir)

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()

        # backup has missing database_map and that is legal
        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-exclude=db5", "--db-exclude=db9"])
            self.assertEqual(
                1, 0,
                "Expecting Error because user do not have pg_database access.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} doesn't contain a database_map, "
                "partial restore is impossible.".format(
                    backup_id), e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        try:
            self.restore_node(
                backup_dir, 'node', node_restored,
                options=["--db-include=db1"])
            self.assertEqual(
                1, 0,
                "Expecting Error because user do not have pg_database access.\n "
                "Output: {0} \n CMD: {1}".format(
                    self.output, self.cmd))
        except ProbackupException as e:
            self.assertIn(
                "ERROR: Backup {0} doesn't contain a database_map, "
                "partial restore is impossible.".format(
                    backup_id), e.message,
                '\n Unexpected Error Message: {0}\n CMD: {1}'.format(
                    repr(e.message), self.cmd))

        # check that simple restore is still possible
        self.restore_node(backup_dir, 'node', node_restored)

        pgdata_restored = self.pgdata_content(node_restored.data_dir)
        self.compare_pgdata(pgdata, pgdata_restored)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_stream_restore_command_option(self):
        """
        correct handling of restore command options
        when restoring STREAM backup

        1. Restore STREAM backup with --restore-command only
        parameter, check that PostgreSQL recovery uses
        restore_command to obtain WAL from archive.

        2. Restore STREAM backup wuth --restore-command
        as replica, check that PostgreSQL recovery uses
        restore_command to obtain WAL from archive.
        """
        fname = self.id().split('.')[3]
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'],
            pg_options={'max_wal_size': '32MB'})

        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(node.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(node.data_dir, 'recovery.conf')

        # Take FULL
        self.backup_node(
            backup_dir, 'node', node, options=['--stream'])

        node.pgbench_init(scale=5)

        node.safe_psql(
            'postgres',
            'create table t1()')

        # restore backup
        node.cleanup()
        shutil.rmtree(os.path.join(node.logs_dir))

        restore_cmd = self.get_restore_command(backup_dir, 'node', node)

        self.restore_node(
            backup_dir, 'node', node,
            options=[
                '--restore-command={0}'.format(restore_cmd)])

        self.assertTrue(
            os.path.isfile(recovery_conf),
            "File '{0}' do not exists".format(recovery_conf))

        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_signal = os.path.join(node.data_dir, 'recovery.signal')
            self.assertTrue(
                os.path.isfile(recovery_signal),
                "File '{0}' do not exists".format(recovery_signal))

        node.slow_start()

        node.safe_psql(
            'postgres',
            'select * from t1')

        timeline_id = node.safe_psql(
            'postgres',
            'select timeline_id from pg_control_checkpoint()').decode('utf-8').rstrip()

        self.assertEqual('2', timeline_id)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_primary_conninfo(self):
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

        # Take FULL
        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        node.pgbench_init(scale=1)

        #primary_conninfo = 'host=192.168.1.50 port=5432 user=foo password=foopass'

        replica = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'replica'))
        replica.cleanup()
        str_conninfo='host=192.168.1.50 port=5432 user=foo password=foopass'

        self.restore_node(
            backup_dir, 'node', replica,
            options=['-R', '--primary-conninfo={0}'.format(str_conninfo)])

        if self.get_version(node) >= self.version_to_num('12.0'):
            standby_signal = os.path.join(replica.data_dir, 'standby.signal')
            self.assertTrue(
                os.path.isfile(standby_signal),
                "File '{0}' do not exists".format(standby_signal))

        # TODO update test
        if self.get_version(node) >= self.version_to_num('12.0'):
            recovery_conf = os.path.join(replica.data_dir, 'postgresql.auto.conf')
            with open(recovery_conf, 'r') as f:
                print(f.read())
        else:
            recovery_conf = os.path.join(replica.data_dir, 'recovery.conf')

        with open(os.path.join(replica.data_dir, recovery_conf), 'r') as f:
            recovery_conf_content = f.read()

        self.assertIn(str_conninfo, recovery_conf_content)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_primary_slot_info(self):
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

        # Take FULL
        self.backup_node(backup_dir, 'node', node, options=['--stream'])

        node.pgbench_init(scale=1)

        replica = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'replica'))
        replica.cleanup()

        node.safe_psql(
            "SELECT pg_create_physical_replication_slot('master_slot')")

        self.restore_node(
            backup_dir, 'node', replica,
            options=['-R', '--primary-slot-name=master_slot'])

        self.set_auto_conf(replica, {'port': replica.port})
        self.set_auto_conf(replica, {'hot_standby': 'on'})

        if self.get_version(node) >= self.version_to_num('12.0'):
            standby_signal = os.path.join(replica.data_dir, 'standby.signal')
            self.assertTrue(
                os.path.isfile(standby_signal),
                "File '{0}' do not exists".format(standby_signal))

        replica.slow_start(replica=True)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_issue_249(self):
        """
        https://github.com/postgrespro/pg_probackup/issues/249
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

        node.safe_psql(
            'postgres',
            'CREATE database db1')

        node.pgbench_init(scale=5)

        node.safe_psql(
            'postgres',
            'CREATE TABLE t1 as SELECT * from pgbench_accounts where aid > 200000 and aid < 450000')

        node.safe_psql(
            'postgres',
            'DELETE from pgbench_accounts where aid > 200000 and aid < 450000')

        node.safe_psql(
            'postgres',
            'select * from pgbench_accounts')

        # FULL backup
        self.backup_node(backup_dir, 'node', node)

        node.safe_psql(
            'postgres',
            'INSERT INTO pgbench_accounts SELECT * FROM t1')

        # restore FULL backup
        node_restored_1 = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored_1'))
        node_restored_1.cleanup()

        self.restore_node(
            backup_dir, 'node',
            node_restored_1, options=["--db-include=db1"])

        self.set_auto_conf(
            node_restored_1,
            {'port': node_restored_1.port, 'hot_standby': 'off'})

        node_restored_1.slow_start()

        node_restored_1.safe_psql(
            'db1',
            'select 1')

        try:
            node_restored_1.safe_psql(
                'postgres',
                'select 1')
        except QueryException as e:
            self.assertIn('FATAL', e.message)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_pg_12_probackup_recovery_conf_compatibility(self):
        """
        https://github.com/postgrespro/pg_probackup/issues/249

        pg_probackup version must be 12 or greater
        """

        if self.pg_config_version < self.version_to_num('12.0'):
           return unittest.skip('You need PostgreSQL >= 12 for this test')

        if self.version_to_num(self.old_probackup_version) >= self.version_to_num('2.4.5'):
            self.assertTrue(False, 'You need pg_probackup < 2.4.5 for this test')

        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        self.backup_node(backup_dir, 'node', node, old_binary=True)

        node.pgbench_init(scale=5)

        node.safe_psql(
            'postgres',
            'CREATE TABLE t1 as SELECT * from pgbench_accounts where aid > 200000 and aid < 450000')

        time = node.safe_psql(
            'SELECT current_timestamp(0)::timestamptz;').decode('utf-8').rstrip()

        node.safe_psql(
            'postgres',
            'DELETE from pgbench_accounts where aid > 200000 and aid < 450000')

        node.cleanup()

        self.restore_node(
            backup_dir, 'node',node,
            options=[
                    "--recovery-target-time={0}".format(time),
                    "--recovery-target-action=promote"],
            old_binary=True)

        node.slow_start()

        self.backup_node(backup_dir, 'node', node, old_binary=True)

        node.pgbench_init(scale=5)

        xid = node.safe_psql(
            'SELECT txid_current()').decode('utf-8').rstrip()
        node.pgbench_init(scale=1)

        node.cleanup()

        self.restore_node(
            backup_dir, 'node',node,
            options=[
                    "--recovery-target-xid={0}".format(xid),
                    "--recovery-target-action=promote"])

        node.slow_start()

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_drop_postgresql_auto_conf(self):
        """
        https://github.com/postgrespro/pg_probackup/issues/249

        pg_probackup version must be 12 or greater
        """

        if self.pg_config_version < self.version_to_num('12.0'):
           return unittest.skip('You need PostgreSQL >= 12 for this test')

        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        self.backup_node(backup_dir, 'node', node)

        # drop postgresql.auto.conf
        auto_path = os.path.join(node.data_dir, "postgresql.auto.conf")
        os.remove(auto_path)

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        node.cleanup()

        self.restore_node(
            backup_dir, 'node',node,
            options=[
                    "--recovery-target=latest",
                    "--recovery-target-action=promote"])

        node.slow_start()

        self.assertTrue(os.path.exists(auto_path))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    def test_truncate_postgresql_auto_conf(self):
        """
        https://github.com/postgrespro/pg_probackup/issues/249

        pg_probackup version must be 12 or greater
        """

        if self.pg_config_version < self.version_to_num('12.0'):
           return unittest.skip('You need PostgreSQL >= 12 for this test')

        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        self.backup_node(backup_dir, 'node', node)

        # truncate postgresql.auto.conf
        auto_path = os.path.join(node.data_dir, "postgresql.auto.conf")
        with open(auto_path, "w+") as f:
            f.truncate()

        self.backup_node(backup_dir, 'node', node, backup_type='page')

        node.cleanup()

        self.restore_node(
            backup_dir, 'node',node,
            options=[
                    "--recovery-target=latest",
                    "--recovery-target-action=promote"])
        node.slow_start()

        self.assertTrue(os.path.exists(auto_path))

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_concurrent_restore(self):
        """"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        node.pgbench_init(scale=1)

        # FULL backup
        self.backup_node(
            backup_dir, 'node', node,
            options=['--stream', '--compress'])

        pgbench = node.pgbench(options=['-T', '7', '-c', '1', '--no-vacuum'])
        pgbench.wait()

        # DELTA backup
        self.backup_node(
            backup_dir, 'node', node, backup_type='delta',
            options=['--stream', '--compress', '--no-validate'])

        pgdata1 = self.pgdata_content(node.data_dir)

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))

        node.cleanup()
        node_restored.cleanup()

        gdb = self.restore_node(
            backup_dir, 'node', node, options=['--no-validate'], gdb=True)

        gdb.set_breakpoint('restore_data_file')
        gdb.run_until_break()

        self.restore_node(
            backup_dir, 'node', node_restored, options=['--no-validate'])

        gdb.remove_all_breakpoints()
        gdb.continue_execution_until_exit()

        pgdata2 = self.pgdata_content(node.data_dir)
        pgdata3 = self.pgdata_content(node_restored.data_dir)

        self.compare_pgdata(pgdata1, pgdata2)
        self.compare_pgdata(pgdata2, pgdata3)

        # Clean after yourself
        self.del_test_dir(module_name, fname)

    # @unittest.skip("skip")
    def test_restore_issue_313(self):
        """
        Check that partially restored PostgreSQL instance cannot be started
        """
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, module_name, fname, 'backup')
        node = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node'),
            set_replication=True,
            initdb_params=['--data-checksums'])

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.slow_start()

        # FULL backup
        backup_id = self.backup_node(backup_dir, 'node', node)
        node.cleanup()

        count = 0
        filelist = self.get_backup_filelist(backup_dir, 'node', backup_id)
        for file in filelist:
            # count only nondata files
            if int(filelist[file]['is_datafile']) == 0 and int(filelist[file]['size']) > 0:
                count += 1

        node_restored = self.make_simple_node(
            base_dir=os.path.join(module_name, fname, 'node_restored'))
        node_restored.cleanup()
        self.restore_node(backup_dir, 'node', node_restored)

        gdb = self.restore_node(backup_dir, 'node', node, gdb=True, options=['--progress'])
        gdb.verbose = False
        gdb.set_breakpoint('restore_non_data_file')
        gdb.run_until_break()
        gdb.continue_execution_until_break(count - 2)
        gdb.quit()

        # emulate the user or HA taking care of PG configuration
        for fname in os.listdir(node_restored.data_dir):
            if fname.endswith('.conf'):
                os.rename(
                    os.path.join(node_restored.data_dir, fname),
                    os.path.join(node.data_dir, fname))

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
                "PANIC:  could not read from control file: read 0 bytes",
                f.read())

        # Clean after yourself
        self.del_test_dir(module_name, fname)
