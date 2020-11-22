-- Test rollback at statement level with nested write statements from a function
LOAD 'pg_statement_rollback.so';
SET pg_statement_rollback.enabled TO on;

DROP SCHEMA IF EXISTS testrsl CASCADE;
CREATE SCHEMA testrsl;

SET search_path TO testrsl,public;

SET log_min_duration_statement TO -1;
SET log_statement TO 'all';
SET log_duration TO off;
SET client_min_messages TO LOG;

CREATE FUNCTION test_insert_fail() RETURNS integer AS $$
BEGIN
    INSERT INTO tbl_rsl VALUES (2, 'two');
    INSERT INTO tbl_rsl VALUES ('three', 3);
    RETURN 1;
END
$$ LANGUAGE plpgsql;

\echo Test internal automatic savepoint with a failing DO block
BEGIN;
DROP TABLE IF EXISTS tbl_rsl;
CREATE TABLE tbl_rsl(id integer, val varchar(256));
INSERT INTO tbl_rsl VALUES (1, 'one');
DO $$
BEGIN
        PERFORM test_insert_fail();
END $$;
DELETE FROM tbl_rsl WHERE id = 1;
INSERT INTO tbl_rsl VALUES (4, 'four');
UPDATE tbl_rsl SET id = 'two', val = 2 WHERE id = 1; -- will fail
SELECT * FROM tbl_rsl; -- Should show record 4
COMMIT;

DROP SCHEMA testrsl CASCADE;
