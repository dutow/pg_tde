
# Test pg_tde_rewind when the database principal key has been rotated
# on the target after divergence.
#
# After divergence, the target (old primary) rotates its database
# principal key. The source (promoted standby) still has the old key.
# Both sides use the same keyring provider (but different active keys).
#
# pg_tde_rewind should still work correctly because:
# - In local mode: each side uses its own provider to decrypt its own keys
# - In remote mode: the keyring still contains both old and new keys,
#   and the key file header tells which key to use

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

	my $db_keyring = "/tmp/pg_tde_rewind_divkey_${test_mode}_db.per";
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

	# Create tde_heap table while standby is running
	primary_psql("CREATE TABLE tde_tbl (id int, data text) USING tde_heap");
	primary_psql(
		"INSERT INTO tde_tbl SELECT g, 'data: ' || g FROM generate_series(1, 200) g");
	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, before promotion')");
	primary_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# --- KEY DIVERGENCE ---
	# Rotate the database principal key on the old primary (target).
	# This creates a new key in the same keyring and re-encrypts all
	# InternalKeys with it.
	primary_psql(
		"SELECT pg_tde_create_key_using_database_key_provider('db-key-rotated', 'db-kp')");
	primary_psql(
		"SELECT pg_tde_set_key_using_database_key_provider('db-key-rotated', 'db-kp')");

	# Divergence writes — UPDATE to dirty a page for pg_rewind to copy
	primary_psql(
		"UPDATE tde_tbl SET data = 'rotated on primary' WHERE id = 1");
	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, after promotion')");

	standby_psql(
		"INSERT INTO tde_tbl VALUES (201, 'in standby, after promotion')");
	standby_psql(
		"INSERT INTO diverge_tbl VALUES ('in standby, after promotion')");

	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# After rewind, data should be correct despite the principal key
	# divergence. The re-encryption correctly loads each side's keys
	# using that side's provider.
	check_query(
		'SELECT count(*) FROM tde_tbl',
		qq(201
),
		'tde_tbl row count after rewind with diverged keys');

	check_query(
		'SELECT data FROM tde_tbl WHERE id = 1',
		qq(data: 1
),
		'tde_tbl pre-divergence row (re-encrypted page)');

	check_query(
		'SELECT data FROM tde_tbl WHERE id = 201',
		qq(in standby, after promotion
),
		'tde_tbl source-only row');

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
