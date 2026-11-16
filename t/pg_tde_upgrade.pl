use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_tde_upgrade');
program_version_ok('pg_tde_upgrade');
program_options_handling_ok('pg_tde_upgrade');

# Make sure delete_old_cluster.{sh,bat} is not generated in the source
# directory.
chdir $PostgreSQL::Test::Utils::tmp_check;

my $oldnotde = PostgreSQL::Test::Cluster->new('oldnotde');
$oldnotde->init;
$oldnotde->start;
$oldnotde->safe_psql(
	'postgres', "
CREATE TABLE test_plain (k int, PRIMARY KEY (k));

INSERT INTO test_plain (k) VALUES (1), (2);
");
$oldnotde->stop;

my $newnotde = PostgreSQL::Test::Cluster->new('newnotde');
$newnotde->init;

command_ok(
	[
		'pg_tde_upgrade', '--no-sync',
		'--old-datadir' => $oldnotde->data_dir,
		'--new-datadir' => $newnotde->data_dir,
		'--old-bindir' => $oldnotde->config_data('--bindir'),
		'--new-bindir' => $newnotde->config_data('--bindir'),
		'--socketdir' => $newnotde->host,
		'--old-port' => $oldnotde->port,
		'--new-port' => $newnotde->port,
	],
	'executes pg_upgrade successfully without pg_tde');

$newnotde->start;
is($newnotde->safe_psql('postgres', "SELECT * FROM test_plain"),
	"1\n2", 'can read tables');
$newnotde->stop;

my $keydir = PostgreSQL::Test::Utils::tempdir;

my $old = PostgreSQL::Test::Cluster->new('old');
$old->init;
$old->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$old->start;
$old->safe_psql(
	'postgres', "
CREATE EXTENSION pg_tde;

SELECT pg_tde_add_global_key_provider_file('file-vault', '$keydir/global.keys');
SELECT pg_tde_create_key_using_global_key_provider('server-key', 'file-vault');
SELECT pg_tde_set_key_using_global_key_provider('server-key', 'file-vault');
SELECT pg_tde_set_server_key_using_global_key_provider('server-key', 'file-vault');
ALTER SYSTEM SET pg_tde.wal_encrypt = on;

CREATE TABLE test_enc (k int, PRIMARY KEY (k)) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1), (2);
");
$old->restart;
$old->stop;

my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;
$new->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$new->append_conf('postgresql.conf', "pg_tde.wal_encrypt = on");

# Failed key copy has to abort the upgrade and keep the existing destination
my @upgrade_cmd = (
	'pg_tde_upgrade', '--no-sync',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port,);
my $unreadable = $old->data_dir . '/pg_tde/unreadable';
my $marker = $new->data_dir . '/pg_tde/marker';

mkdir $new->data_dir . '/pg_tde';
append_to_file($marker, 'x');
append_to_file($unreadable, 'x');
chmod 0000, $unreadable;

command_fails([@upgrade_cmd], 'fails when pg_tde can not be copied');
ok(-f $marker, 'existing pg_tde directory kept after failed copy');
ok(!-e $new->data_dir . '/pg_upgrade_output.d', 'pg_upgrade not started after failed copy');

unlink $unreadable;

command_ok(
	[@upgrade_cmd],
	'executes pg_upgrade successfully with pg_tde');

ok(!-e $marker, 'old pg_tde directory replaced');

$new->start;
is($new->safe_psql('postgres', "SELECT * FROM test_enc"),
	"1\n2", 'can read encrypted tables');
$new->stop;

done_testing();
