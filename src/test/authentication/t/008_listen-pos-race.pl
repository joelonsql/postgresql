# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(usleep);
use Test::More;

if ($ENV{enable_injection_points} ne 'yes') {
    plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new("test");
$node->init;
$node->start;


if (!$node->check_extension('injection_points')) {
    plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

my $listener = $node->background_psql('postgres');
my $listener_with_invalid_pos = $node->background_psql('postgres');
my $notifying_psql = $node->background_psql('postgres');

# setup injection points
$node->safe_psql('postgres', "SELECT injection_points_attach('listen-notify-signal-backends', 'wait');");

$listener_with_invalid_pos->query_safe(
    qq[
     SELECT injection_points_set_local();
     SELECT injection_points_attach('listen-notify-local-pos', 'wait');
 ]);

# listener starts listening to channel ch, and starts a transaction,
# while listener is within an active transaction, its pos will be 0,
# and therefore all new listeners will have to start reading from 0 position too because of it.
$listener->query_safe("LISTEN ch;");
$listener->query_safe("BEGIN;");

# create a lot of notifications and start waiting
# on listen-notify-signal-backends (before direct advancement)
$notifying_psql->query_until(
    qr/start/, q(
   \echo start
   BEGIN;
   select pg_notify('ch', repeat('a',3000) || x::text) from generate_series(1,100) as x;
   COMMIT;

));
# Wait until notifying_psql enters the wait injection point.
$node->wait_for_event('client backend',	'listen-notify-signal-backends');

# listener_with_invalid_pos executes LISTEN command and starts
# waiting on the injection point with pos = 0 in local variable
$listener_with_invalid_pos->query_until(
    qr/start/, q(
   \echo start
   LISTEN ch2;
));
# Wait until listener_with_invalid_pos enters the wait injection point.
$node->wait_for_event('client backend',	'listen-notify-local-pos');

# wake up notifying backend. direct advancement should be applied now to listener_with_invalid_pos
# listener_with_invalid_pos is still waiting on injection point with local pos = 0
$node->safe_psql('postgres', "SELECT injection_points_wakeup('listen-notify-signal-backends');");

#sleep just to be sure that direct advancement was applied to listener_with_invalid_pos
sleep 1;

# let listener to advance its position to unblock queue truncating
$listener->query_safe("COMMIT");

# truncate the queue
$node->safe_psql('postgres', "select pg_notification_queue_usage();");


# wake up listener_with_invalid_pos with local copy of pos = 0. queue has been truncated already.
# test should fail after it. 008_listen-pos-race_test.log contains error details
$node->safe_psql('postgres',"SELECT injection_points_wakeup('listen-notify-local-pos');");


$listener->quit();
$listener_with_invalid_pos->quit();
$notifying_psql->quit();

done_testing();
