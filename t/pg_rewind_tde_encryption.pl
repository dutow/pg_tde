
# Test pg_tde_rewind with encrypted (tde_heap) tables.
#
# Background:
# When a tde_heap table is created while a standby is replicating,
# tde_smgr_create_key_redo generates a DIFFERENT random InternalKey
# on the standby (K2) than the one on the primary (K1). During normal
# operation this is fine: WAL full-page images carry plaintext and each
# node re-encrypts with its own key via smgr.
#
# The pg_rewind bug:
# After divergence and rewind, blocks copied from the source are
# K2-encrypted. On the target, either:
#  (a) The key map was also copied from source (K2) but unchanged
#      pages remain K1-encrypted -> corruption reading those pages, or
#  (b) The key map was NOT copied (target keeps K1) but copied pages
#      are K2-encrypted -> corruption reading those pages.
#
# For remote mode pg_read_binary_file bypasses smgr on the source,
# returning raw K2-encrypted bytes with no re-encryption path.
# Even when WAL replay via streaming is available, it only provides
# FPIs for pages the SOURCE modified after divergence. Pages the
# TARGET modified (triggering pg_rewind to copy them) but the SOURCE
# did not modify have no FPIs, so the K2-encrypted copy persists.
#
# This test uses an UPDATE on an early row (page 0) on the old
# primary to force pg_rewind to copy that page from the source.
# The source never modifies page 0, so there is no FPI to mask the
# problem. A CHECKPOINT on the standby ensures encrypted pages are
# flushed to disk before pg_read_binary_file reads them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster($test_mode);
	RewindTest::start_primary();

	my $db_keyring = "/tmp/pg_tde_rewind_tde_enc_${test_mode}_db.per";
	unlink($db_keyring);

	primary_psql(
		"SELECT pg_tde_add_database_key_provider_file('db-kp', '${db_keyring}')");
	primary_psql(
		"SELECT pg_tde_create_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql(
		"SELECT pg_tde_set_key_using_database_key_provider('db-key', 'db-kp')");

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('in primary')");

	primary_psql("CHECKPOINT");

	RewindTest::create_standby($test_mode);

	# Create tde_heap table AFTER standby is running.
	# Standby generates a DIFFERENT InternalKey (K2 vs primary's K1).
	primary_psql("CREATE TABLE tde_tbl (id int, data text) USING tde_heap");
	primary_psql(
		"INSERT INTO tde_tbl SELECT g, 'before divergence: ' || g FROM generate_series(1, 500) g"
	);

	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, before promotion')");
	primary_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# UPDATE an early row on the old primary to dirty page 0.
	# pg_rewind will see this in the target's WAL and copy page 0
	# from the source. The source never modifies page 0 after
	# divergence, so there is NO full-page image in the source WAL
	# that could fix the page via replay.
	primary_psql(
		"UPDATE tde_tbl SET data = 'modified on old primary' WHERE id = 1");
	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, after promotion')");

	standby_psql(
		"INSERT INTO diverge_tbl VALUES ('in standby, after promotion')");
	standby_psql(
		"INSERT INTO tde_tbl VALUES (502, 'in standby, after promotion')");

	# Flush K2-encrypted pages to disk on the source so that
	# pg_read_binary_file returns real encrypted data (not zeros).
	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	check_query(
		'SELECT count(*) FROM tde_tbl',
		qq(501
),
		'tde_tbl row count after rewind');

	check_query(
		'SELECT data FROM tde_tbl WHERE id = 1',
		qq(before divergence: 1
),
		'tde_tbl pre-divergence data integrity');

	check_query(
		'SELECT data FROM tde_tbl WHERE id = 502',
		qq(in standby, after promotion
),
		'tde_tbl post-divergence data from source');

	check_query(
		'SELECT * FROM diverge_tbl ORDER BY d',
		qq(in primary
in primary, before promotion
in standby, after promotion
),
		'diverge_tbl content after rewind');

	RewindTest::clean_rewind_test();
	return;
}

run_test('local');
run_test('remote');

done_testing();
