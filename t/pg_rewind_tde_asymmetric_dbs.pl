
# Test pg_tde_rewind with asymmetric encrypted databases.
#
# Scenario 1 (source-only): After promotion, the source (promoted standby)
# creates and encrypts a database that doesn't exist on the target.
# After rewind, the target should have this database with readable data.
#
# Scenario 2 (target-only): After promotion, the target (old primary)
# creates and encrypts a database that doesn't exist on the source.
# After rewind, this database should be gone (pg_rewind removes it).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(run);

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

# Like check_query but allows specifying the database name
sub check_query_db
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($dbname, $query, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);

	my $result = run [
		'psql', '-q', '-A', '-t', '--no-psqlrc', '-d',
		$node_primary->connstr($dbname),
		'-c', $query
	  ],
	  '>', \$stdout, '2>', \$stderr;

	is($result, 1, "$test_name: psql exit code");
	is($stderr, '', "$test_name: psql no stderr");
	is($stdout, $expected_stdout, "$test_name: query result matches");
}

sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster($test_mode);
	RewindTest::start_primary();

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('in primary')");
	primary_psql("CHECKPOINT");

	RewindTest::create_standby($test_mode);

	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, before promotion')");
	primary_psql('CHECKPOINT');

	RewindTest::promote_standby();

	# --- Source-only encrypted database ---
	my $src_keyring = "/tmp/pg_tde_rewind_asymm_${test_mode}_src.per";
	unlink($src_keyring);

	standby_psql("CREATE DATABASE src_enc_db");
	standby_psql("CREATE EXTENSION pg_tde", 'src_enc_db');
	standby_psql(
		"SELECT pg_tde_add_database_key_provider_file('file-kp', '${src_keyring}')",
		'src_enc_db');
	standby_psql(
		"SELECT pg_tde_create_key_using_database_key_provider('db-key', 'file-kp')",
		'src_enc_db');
	standby_psql(
		"SELECT pg_tde_set_key_using_database_key_provider('db-key', 'file-kp')",
		'src_enc_db');
	standby_psql(
		"CREATE TABLE src_tbl (id int, data text) USING tde_heap",
		'src_enc_db');
	standby_psql(
		"INSERT INTO src_tbl SELECT g, 'source data: ' || g FROM generate_series(1, 100) g",
		'src_enc_db');

	# --- Target-only encrypted database ---
	my $tgt_keyring = "/tmp/pg_tde_rewind_asymm_${test_mode}_tgt.per";
	unlink($tgt_keyring);

	primary_psql("CREATE DATABASE tgt_enc_db");
	primary_psql("CREATE EXTENSION pg_tde", 'tgt_enc_db');
	primary_psql(
		"SELECT pg_tde_add_database_key_provider_file('file-kp', '${tgt_keyring}')",
		'tgt_enc_db');
	primary_psql(
		"SELECT pg_tde_create_key_using_database_key_provider('db-key', 'file-kp')",
		'tgt_enc_db');
	primary_psql(
		"SELECT pg_tde_set_key_using_database_key_provider('db-key', 'file-kp')",
		'tgt_enc_db');
	primary_psql(
		"CREATE TABLE tgt_tbl (id int, data text) USING tde_heap",
		'tgt_enc_db');
	primary_psql(
		"INSERT INTO tgt_tbl SELECT g, 'target data: ' || g FROM generate_series(1, 50) g",
		'tgt_enc_db');

	# Divergence writes
	primary_psql(
		"INSERT INTO diverge_tbl VALUES ('in primary, after promotion')");
	standby_psql(
		"INSERT INTO diverge_tbl VALUES ('in standby, after promotion')");

	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# Verify source-only database exists with correct data
	check_query_db('src_enc_db',
		'SELECT count(*) FROM src_tbl',
		qq(100\n),
		'src_enc_db row count');

	check_query_db('src_enc_db',
		'SELECT data FROM src_tbl WHERE id = 1',
		qq(source data: 1\n),
		'src_enc_db data integrity');

	# Verify target-only database is gone
	check_query(
		"SELECT datname FROM pg_database WHERE datname = 'tgt_enc_db'",
		qq(),
		'tgt_enc_db removed after rewind');

	# Verify diverge_tbl
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
