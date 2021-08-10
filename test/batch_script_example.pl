#!/usr/bin/perl

use DBI;
use Benchmark;

print "Creating the regression database.\n";
my $dbh = DBI->connect("dbi:Pg:host=localhost;dbname=template1", '', '', {AutoCommit => 1});
die "ERROR: can't connect to database template1\n" if (not defined $dbh);
$dbh->do("DROP DATABASE contrib_regression");
$dbh->do("CREATE DATABASE contrib_regression");
$dbh->do("ALTER DATABASE contrib_regression SET lc_messages = 'C'");
$dbh->disconnect;

print "Connect to the regression database.\n";
$dbh = DBI->connect("dbi:Pg:host=localhost;dbname=contrib_regression", '', '', {AutoCommit => 1, PrintError => 0});
die "ERROR: can't connect to database contrib_regression\n" if (not defined $dbh);
print "---------------------------------------------\n";
print "Create the extension and initialize the test\n";
print "---------------------------------------------\n";
$dbh->do("LOAD 'pg_statement_rollback.so'") or die "FATAL: " . $dbh->errstr . "\n";
$dbh->do("SET pg_statement_rollback.savepoint_name TO 'aze'") or die "FATAL: " . $dbh->errstr . "\n";
$dbh->do("SET pg_statement_rollback.enable_writeonly TO on") or die "FATAL: " . $dbh->errstr . "\n";
$dbh->do("CREATE TABLE t1 (a bigint PRIMARY KEY, lbl text)") or die "FATAL: " . $dbh->errstr . "\n";
#$dbh->do("SET log_min_messages TO DEBUG1") or die "FATAL: " . $dbh->errstr . "\n";
$dbh->do("SET pg_statement_rollback.enabled TO on") or die "FATAL: " . $dbh->errstr . "\n";
$dbh->do("BEGIN") or die "FATAL: " . $dbh->errstr . "\n";
print "---------------------------------------------\n";
print "Start DML work\n";
print "---------------------------------------------\n";
my $start = new Benchmark;
my $sth = $dbh->prepare("INSERT INTO t1 VALUES (?, ?)");
if (not defined $sth) {
	print STDERR "PREPARE ERROR: " . $dbh->errstr . "\n";
}
for (my $i = 0; $i <= 10000; $i++)
{
	#$dbh->do("SAVEPOINT foo") or die "FATAL: " . $dbh->errstr . "\n";
	# Generate a duplicate key each two row inserted
	my $val = $i;
	$val = $i-1 if ($i % 2 != 0);
	unless ($sth->execute($val, 'insert '.$i)) {
		#$dbh->do("ROLLBACK TO foo") or die "FATAL: " . $dbh->errstr . "\n";
		#print STDERR "EXECUTE ERROR: " . $dbh->errstr . "\n";
	}
	#else {
		#$dbh->do("RELEASE foo") or die "FATAL: " . $dbh->errstr . "\n";
	#}
}
$sth->finish();
my $end = new Benchmark;

$dbh->do("COMMIT;");

print "---------------------------------------------\n";
print "Look at inserted values in DML table\n";
print "---------------------------------------------\n";
my $sth = $dbh->prepare("SELECT 5001 as expected, count(*) FROM t1");
$sth->execute();
while (my $row = $sth->fetch) {
	print "Expected: $row->[0], Count:  $row->[1]\n";
}

$dbh->disconnect;

my $td = timediff($end, $start);
print "DML insert took: " . timestr($td) . "\n";

exit 0;
