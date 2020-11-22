\set VERBOSITY default
\set ECHO all
LOAD 'pg_statement_rollback.so';
SET pg_statement_rollback.enabled = 1;
BEGIN;
CREATE TABLE savepoint_test(id integer);
INSERT INTO savepoint_test SELECT 1;
SELECT COUNT( * ) FROM savepoint_test; -- Return 1
INSERT INTO savepoint_test SELECT 1;
SAVEPOINT useless;
INSERT INTO savepoint_test SELECT 1;
SAVEPOINT s1;
INSERT INTO savepoint_test SELECT 1;
SELECT COUNT( * ) FROM savepoint_test; -- Return 4
INSERT INTO savepoint_test SELECT 'wrong 1';
SELECT COUNT( * ) FROM savepoint_test;
INSERT INTO savepoint_test SELECT 'wrong 2';
ROLLBACK TO s1;
SELECT COUNT( * ) FROM savepoint_test; -- Return 3
RELEASE badsp;
SELECT COUNT( * ) FROM savepoint_test; -- Return 3
RELEASE useless;
INSERT INTO savepoint_test SELECT 1;
SELECT COUNT( * ) FROM savepoint_test; -- Return 4
ROLLBACK;

