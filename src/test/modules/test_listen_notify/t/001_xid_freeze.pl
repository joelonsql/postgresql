# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test that VACUUM FREEZE don't remove clog files that are needed to check the
# transaction status of notifications that are on the LISTEN/NOTIFY queue
# during its execution. The VACUUM FREEZE operation should check the oldest xid
# on the queue during execution.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# Check if the extension xid_wraparound is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('xid_wraparound'))
{
	plan skip_all => 'Extension xid_wraparound not installed';
}

# Setup
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound');
$node->safe_psql('postgres',
	'CREATE TABLE t AS SELECT g AS a, g+2 AS b from generate_series(1,10) g;'
);
$node->safe_psql('postgres',
	'ALTER DATABASE template0 WITH ALLOW_CONNECTIONS true');

# --- Start Session 1 and leave it idle in transaction
my $psql_session1 = $node->background_psql('postgres');
$psql_session1->query_safe('listen s;', "Session 1 listens to 's'");
$psql_session1->query_safe('begin;', "Session 1 starts a transaction");

# --- Session 2, multiple notify's, and commit ---
for my $i (1 .. 10)
{
	$node->safe_psql(
		'postgres', "
		BEGIN;
		NOTIFY s, '$i';
		COMMIT;");
}

# Consume enough XIDs to trigger truncation
$node->safe_psql('postgres', 'select consume_xids(1050000);');

# Execute update so the frozen xid of "t" table is updated to a xid greater
# than consume_xids() result
$node->safe_psql('postgres', 'UPDATE t SET a = a+b;');

# Remember current datfrozenxid before vacuum freeze to ensure that it is advanced.
my $datafronzenxid = $node->safe_psql('postgres', "select datfrozenxid from pg_database where datname = 'postgres'");

# Execute vacuum freeze on all databases
$node->command_ok([ 'vacuumdb', '--all', '--freeze', '--port', $node->port ],
	"vacuumdb --all --freeze");

# Get the new datfrozenxid after vacuum freeze to ensure that is not advanced
# to a value greater than the xid used to send the notifications.
my $datafronzenxid_freeze = $node->safe_psql('postgres', "select datfrozenxid from pg_database where datname = 'postgres'");
print("\n\n$datafronzenxid < $datafronzenxid_freeze\n\n");
ok($datafronzenxid < $datafronzenxid_freeze, 'datfrozenxid is not fully advanced');

# On Session 1, commit and ensure that the all notifications is received
my $res = $psql_session1->query_safe('commit;', "commit listen s;");
my $notifications_count = 0;
foreach my $i (split('\n', $res))
{
	$notifications_count++;
	like($i, qr/Asynchronous notification "s" with payload "$notifications_count" received/);
}
is($notifications_count, 10, 'received all committed notifications');

# Execute vacuum freeze on all databases and ensure that the datafrozenxid is advanced

$datafronzenxid = $node->safe_psql('postgres', "select datfrozenxid from pg_database where datname = 'postgres'");

$node->command_ok([ 'vacuumdb', '--all', '--freeze', '--port', $node->port ],
	"vacuumdb --all --freeze");

$datafronzenxid_freeze = $node->safe_psql('postgres', "select datfrozenxid from pg_database where datname = 'postgres'");

ok($datafronzenxid_freeze > $datafronzenxid, "datfrozenxid is advanced: $datafronzenxid_freeze > $datafronzenxid");

done_testing();
