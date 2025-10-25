# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Path qw(mkpath);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\blisten_notify\b/)
{
	plan skip_all => "test listen_notify not enabled in PG_TEST_EXTRA";
}

# Test checks that listeners do not receive notifications from aborted
# transaction even if notifications have been added to the listen/notify
# queue. To reproduce it we use the fact that serializable conflicts
# are checked after tx adds notifications to the queue.

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# Setup
$node->safe_psql('postgres', 'CREATE TABLE t1 (a bigserial);');

# Listener
my $psql_listener = $node->background_psql('postgres');
$psql_listener->query_safe('LISTEN ch;');

# Session1. Start SERIALIZABLE tx and add a notification.
my $psql_session1 = $node->background_psql('postgres');
$psql_session1->query_safe("
	BEGIN ISOLATION LEVEL SERIALIZABLE;
	SELECT * FROM t1;
	INSERT INTO t1 DEFAULT VALUES;
	NOTIFY ch,'committed_0';
	NOTIFY ch,'committed_1';
");

# Session2. Start SERIALIZABLE tx, add a notification and introduce a conflict
# with session1.
my $psql_session2 = $node->background_psql('postgres', on_error_stop => 0);
$psql_session2->query_safe("
	BEGIN ISOLATION LEVEL SERIALIZABLE;
	SELECT * FROM t1;
	INSERT INTO t1 DEFAULT VALUES;
");

# Send notifications that should not be eventually delivered, as session2
# transaction will be aborted.
my $message = 'aborted_' . 'a' x 1000;
for (my $i = 0; $i < 10; $i++) {
    $psql_session2->query_safe("NOTIFY ch, '$i$message'");
}

# Session1 should be committed successfully. Listeners must receive session1
# notifications.
$psql_session1->query_safe("COMMIT;");

# Session2 should be aborted due to the conflict with session1. Transaction
# is aborted after adding notifications to the listen/notify queue, but
# listeners should not receive session2 notifications.
$psql_session2->query("COMMIT;");

# send more notifications after aborted
$node->safe_psql('postgres', "NOTIFY ch, 'committed_2';");
$node->safe_psql('postgres', "NOTIFY ch, 'committed_3';");

# fetch notifications
my $res = $psql_listener->query_safe('begin; commit;');

# check received notifications
my @lines = split('\n', $res);
is(@lines, 4, 'received all committed notifications');
for (my $i = 0; $i < 4; $i++) {
    like($lines[$i], qr/Asynchronous notification "ch" with payload "committed_$i" received/);
}

ok($psql_listener->quit);
ok($psql_session1->quit);
ok($psql_session2->quit);

done_testing();
