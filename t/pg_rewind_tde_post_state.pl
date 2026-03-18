
# Test that after pg_tde_rewind, the target's TDE state matches the source's.
#
# After rewind, the target should be a clone of the source. For pg_tde
# this means:
#   - Provider configuration (*_providers) matches the source's
#   - Principal key info (key file headers) references the same key as source
#   - WAL key configuration allows reading all necessary WAL
#   - Data in encrypted tables is correct and readable
#
# This test covers several scenarios:
#   A) Baseline: no key changes after divergence (both sides same config)
#   B) Target rotated database principal key after divergence
#   C) Source rotated database principal key after divergence
#   D) Target changed WAL/server key after divergence

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(run);

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

# Compare a file between the primary (after rewind) and the standby (source).
# For local mode, compare files on disk directly.
# Returns 1 if identical, 0 if different.
sub files_match
{
	my ($filename, $test_name) = @_;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $primary_path = $node_primary->data_dir . "/pg_tde/$filename";
	my $standby_path = $node_standby->data_dir . "/pg_tde/$filename";

	if (!-e $primary_path)
	{
		fail("$test_name: primary file $filename exists");
		return 0;
	}
	if (!-e $standby_path)
	{
		fail("$test_name: standby file $filename exists");
		return 0;
	}

	# Compare file contents
	my $primary_data = slurp_file($primary_path);
	my $standby_data = slurp_file($standby_path);

	is($primary_data, $standby_data, $test_name);
}

# Read principal key name from a key file header.
# The header is: int32 file_version + TDESignedPrincipalKeyInfo
# TDESignedPrincipalKeyInfo.data is TDEPrincipalKeyInfo:
#   Oid databaseId(4) + Oid keyringId(4) + [4 padding] +
#   struct timeval creationTime(16) + char name[256]
# So key name is at offset 4 + 4 + 4 + 4 + 16 = 32 in the file.
sub read_key_name_from_file
{
	my ($filepath) = @_;
	my $data = slurp_file($filepath);
	return undef if length($data) < 288;

	my $name = substr($data, 32, 256);
	$name =~ s/\0.*//;  # trim at first null byte
	return $name;
}

sub principal_key_names_match
{
	my ($filename, $test_name) = @_;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $primary_path = $node_primary->data_dir . "/pg_tde/$filename";
	my $standby_path = $node_standby->data_dir . "/pg_tde/$filename";

	my $primary_name = read_key_name_from_file($primary_path) // "(missing)";
	my $standby_name = read_key_name_from_file($standby_path) // "(missing)";

	is($primary_name, $standby_name,
	   "$test_name (primary='$primary_name', source='$standby_name')");
}


# ============================================================
# Scenario A: Baseline — no key changes after divergence
# ============================================================
sub test_baseline
{
	my $test_mode = shift;
	my $scenario = "baseline_$test_mode";

	RewindTest::setup_cluster($scenario);
	RewindTest::start_primary();

	my $db_keyring = "/tmp/pg_tde_rewind_poststate_${scenario}.per";
	unlink($db_keyring);

	primary_psql("SELECT pg_tde_add_database_key_provider_file('db-kp', '$db_keyring')");
	primary_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key', 'db-kp')");

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('baseline')");
	primary_psql("CHECKPOINT");

	RewindTest::create_standby($scenario);

	primary_psql("CREATE TABLE tde_tbl (id int) USING tde_heap");
	primary_psql("INSERT INTO tde_tbl SELECT g FROM generate_series(1,100) g");
	primary_psql("CHECKPOINT");

	RewindTest::promote_standby();

	# No key changes — just diverge
	primary_psql("UPDATE tde_tbl SET id = -1 WHERE id = 1");
	primary_psql("INSERT INTO diverge_tbl VALUES ('primary after')");
	standby_psql("INSERT INTO diverge_tbl VALUES ('standby after')");
	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# Data should be correct
	check_query('SELECT count(*) FROM tde_tbl', qq(100\n),
				"[$scenario] tde_tbl row count");

	# Provider files should match source
	files_match("5_providers",
				"[$scenario] database provider config matches source");

	# Principal key name in key file header should match source
	principal_key_names_match("5_keys",
							 "[$scenario] database principal key name matches source");

	# WAL key principal key name should match source
	principal_key_names_match("wal_keys",
							 "[$scenario] WAL principal key name matches source");

	RewindTest::clean_rewind_test();
}


# ============================================================
# Scenario B: Target rotated database principal key
# ============================================================
sub test_target_rotated_db_key
{
	my $test_mode = shift;
	my $scenario = "tgt_rot_$test_mode";

	RewindTest::setup_cluster($scenario);
	RewindTest::start_primary();

	my $db_keyring = "/tmp/pg_tde_rewind_poststate_${scenario}.per";
	unlink($db_keyring);

	primary_psql("SELECT pg_tde_add_database_key_provider_file('db-kp', '$db_keyring')");
	primary_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key', 'db-kp')");

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('data')");
	primary_psql("CHECKPOINT");

	RewindTest::create_standby($scenario);

	primary_psql("CREATE TABLE tde_tbl (id int) USING tde_heap");
	primary_psql("INSERT INTO tde_tbl SELECT g FROM generate_series(1,100) g");
	primary_psql("CHECKPOINT");

	RewindTest::promote_standby();

	# TARGET rotates database key
	primary_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key-rotated', 'db-kp')");
	primary_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key-rotated', 'db-kp')");

	primary_psql("UPDATE tde_tbl SET id = -1 WHERE id = 1");
	primary_psql("INSERT INTO diverge_tbl VALUES ('primary after')");
	standby_psql("INSERT INTO diverge_tbl VALUES ('standby after')");
	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# Data should be correct
	check_query('SELECT count(*) FROM tde_tbl', qq(100\n),
				"[$scenario] tde_tbl row count");
	check_query('SELECT id FROM tde_tbl WHERE id = 1', qq(1\n),
				"[$scenario] tde_tbl row 1 reverted to source value");

	# After rewind, target should use SOURCE's principal key ('db-key'),
	# not the rotated 'db-key-rotated'.
	principal_key_names_match("5_keys",
							 "[$scenario] database principal key reverted to source's");

	RewindTest::clean_rewind_test();
}


# ============================================================
# Scenario C: Source rotated database principal key
# ============================================================
sub test_source_rotated_db_key
{
	my $test_mode = shift;
	my $scenario = "src_rot_$test_mode";

	RewindTest::setup_cluster($scenario);
	RewindTest::start_primary();

	my $db_keyring = "/tmp/pg_tde_rewind_poststate_${scenario}.per";
	unlink($db_keyring);

	primary_psql("SELECT pg_tde_add_database_key_provider_file('db-kp', '$db_keyring')");
	primary_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key', 'db-kp')");

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('data')");
	primary_psql("CHECKPOINT");

	RewindTest::create_standby($scenario);

	primary_psql("CREATE TABLE tde_tbl (id int) USING tde_heap");
	primary_psql("INSERT INTO tde_tbl SELECT g FROM generate_series(1,100) g");
	primary_psql("CHECKPOINT");

	RewindTest::promote_standby();

	# SOURCE (promoted standby) rotates database key
	standby_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key-src-rotated', 'db-kp')");
	standby_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key-src-rotated', 'db-kp')");

	primary_psql("UPDATE tde_tbl SET id = -1 WHERE id = 1");
	primary_psql("INSERT INTO diverge_tbl VALUES ('primary after')");
	standby_psql("INSERT INTO diverge_tbl VALUES ('standby after')");
	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# Data should be correct
	check_query('SELECT count(*) FROM tde_tbl', qq(100\n),
				"[$scenario] tde_tbl row count");

	# After rewind, target should use SOURCE's rotated key
	principal_key_names_match("5_keys",
							 "[$scenario] database principal key matches source's rotated key");

	RewindTest::clean_rewind_test();
}


# ============================================================
# Scenario D: Target rotated WAL/server key
# ============================================================
sub test_target_rotated_wal_key
{
	my $test_mode = shift;
	my $scenario = "tgt_wal_rot_$test_mode";

	RewindTest::setup_cluster($scenario);
	RewindTest::start_primary();

	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('data')");
	primary_psql("CHECKPOINT");

	RewindTest::create_standby($scenario);

	primary_psql("INSERT INTO diverge_tbl VALUES ('before promotion')");
	primary_psql("CHECKPOINT");

	RewindTest::promote_standby();

	# TARGET rotates the server/WAL key
	primary_psql(
		"SELECT pg_tde_create_key_using_global_key_provider('wal-key-rotated', 'file-keyring-wal')");
	primary_psql(
		"SELECT pg_tde_set_server_key_using_global_key_provider('wal-key-rotated', 'file-keyring-wal')");

	primary_psql("INSERT INTO diverge_tbl VALUES ('primary after')");
	standby_psql("INSERT INTO diverge_tbl VALUES ('standby after')");
	standby_psql("CHECKPOINT");

	RewindTest::run_pg_rewind($test_mode);

	# Data should be correct
	check_query(
		'SELECT * FROM diverge_tbl ORDER BY d',
		qq(before promotion
data
standby after
),
		"[$scenario] diverge_tbl content");

	# After rewind, WAL key should match source's (not the rotated one)
	principal_key_names_match("wal_keys",
							 "[$scenario] WAL key name matches source's");

	RewindTest::clean_rewind_test();
}


# Run all scenarios in local mode (where we can compare files directly)
test_baseline('local');
test_target_rotated_db_key('local');
test_source_rotated_db_key('local');
test_target_rotated_wal_key('local');

done_testing();
