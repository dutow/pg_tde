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
	primary_psql("SELECT pg_tde_add_database_key_provider_file('db-kp', '${db_keyring}')");
	primary_psql("SELECT pg_tde_create_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql("SELECT pg_tde_set_key_using_database_key_provider('db-key', 'db-kp')");
	primary_psql("CREATE TABLE diverge_tbl (d text)");
	primary_psql("INSERT INTO diverge_tbl VALUES ('in primary')");
	primary_psql("CHECKPOINT");
	RewindTest::create_standby($test_mode);
	primary_psql("CREATE TABLE tde_tbl (id int, data text) USING tde_heap");
	primary_psql("INSERT INTO tde_tbl SELECT g, 'before divergence: ' || g FROM generate_series(1, 500) g");
	primary_psql("INSERT INTO diverge_tbl VALUES ('in primary, before promotion')");
	primary_psql('CHECKPOINT');
	RewindTest::promote_standby();
	primary_psql("UPDATE tde_tbl SET data = 'modified on old primary' WHERE id = 1");
	primary_psql("INSERT INTO diverge_tbl VALUES ('in primary, after promotion')");
	standby_psql("INSERT INTO diverge_tbl VALUES ('in standby, after promotion')");
	standby_psql("INSERT INTO tde_tbl VALUES (502, 'in standby, after promotion')");
	standby_psql("CHECKPOINT");
	RewindTest::run_pg_rewind($test_mode);
	check_query('SELECT count(*) FROM tde_tbl', qq(501\n), 'tde_tbl row count after rewind');
	check_query('SELECT data FROM tde_tbl WHERE id = 1', qq(before divergence: 1\n), 'tde_tbl pre-divergence data integrity');
	check_query('SELECT data FROM tde_tbl WHERE id = 502', qq(in standby, after promotion\n), 'tde_tbl post-divergence data from source');
	check_query('SELECT * FROM diverge_tbl ORDER BY d', qq(in primary\nin primary, before promotion\nin standby, after promotion\n), 'diverge_tbl content after rewind');
	RewindTest::clean_rewind_test();
	return;
}
run_test('local');
run_test('remote');
done_testing();
