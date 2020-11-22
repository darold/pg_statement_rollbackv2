## Server side rollback at statement level for PostgreSQL

* [Description](#description)
* [Installation](#installation)
* [Configuration](#configuration)
* [Use of the extension](#use-of-the-extension)
* [Performances](#performances)
* [Problems](#problems)
* [Authors](#authors)
* [License](#license)

### [Description](#description)

pg_statement_rollback is a PostgreSQL extension to add server side
transaction with rollback at statement level like in Oracle or DB2.

If at any time during execution a SQL statement causes an error, all
effects of the statement are rolled back. The effect of the rollback
is as if that statement had never been run. This operation is called
statement-level rollback and has the following characteristics:

- A SQL statement that does not succeed causes the loss only of work
  it would have performed itself. The unsuccessful statement does
  not cause the loss of any work that preceded it in the current
  transaction.
- The effect of the rollback is as if the statement had never been
  run.

In PostgreSQL the transaction cannot continue when you encounter
an error and the entire work done in the transaction is rolled back.
Oracle or DB2 have implicit savepoint before each statement execution
which allow a rollback to the state just before the statement failure.

The pg_statement_rollback extension execute the automatic savepoint at
server side which adds a very limited penalty to the performances
(see "Performances" chapter below). The rollback to the last successful
statement state is entirely driven at server side, the client doesn't
have to take care of the rolling back.

    BEGIN;
    CREATE TABLE savepoint_test(id integer);
    INSERT INTO savepoint_test SELECT 1;
    SELECT COUNT( * ) FROM savepoint_test; -- return 1
    INSERT INTO savepoint_test SELECT 'wrong 1'; -- generate an error
    SELECT COUNT( * ) FROM savepoint_test; -- still return 1
    COMMIT;

Without the extension everything will be cancelled and statement after
the error on INSERT will return:

    ERROR:  current transaction is aborted, commands ignored until end of transaction block

Here is the output of the test with statement-level rollback enabled:

    BEGIN
    CREATE TABLE
    INSERT 0 1
     count 
    -------
         1
    (1 row)
    
    psql:toto.sql:9: ERROR:  invalid input syntax for type integer: "wrong 1"
    LINE 1: INSERT INTO savepoint_test SELECT 'wrong 1';
                                              ^
     count 
    -------
         1
    (1 row)
    
    COMMIT


### [Installation](#installation)

To install the pg_statement_rollback extension you need at least a
PostgreSQL version 14. Untar the pg_statement_rollback tarball
anywhere you want then you'll need to compile it with PGXS.  The
`pg_config` tool must be in your path.

Depending on your installation, you may need to install some devel
package. Once `pg_config` is in your path, do

    make
    sudo make install

To run test execute the following command as superuser:

    make installcheck

### [Configuration](#configuration)

#### Server side automatic savepoint

- *pg_statement_rollback.enabled*

The extension can be enable / disable using this GUC, default is
enabled. To disable the extension use:

    SET pg_statement_rollback.enabled TO off;

You can disable or enable the extension at any moment in a session.

### [Use of the extension](#use-of-the-extension)

In all session where you want to use pg_statement_rollback transaction with
rollback at statement level you will have to load the extension using:

* LOAD 'pg_statement_rollback.so';
* SET pg_statement_rollback.enabled TO on;

Then when an error is encountered in a transaction the extension will
automatically rollback to the state of the last successfull statement
instead of rolling back the entire transaction and will continue the
running transaction just like if there was no error.

If you want to generalize the use of the extension modify your postgresql.conf
file to set

    session_preload_libraries = 'pg_statement_rollback'

and add

    pg_statement_rollback.enabled = on

at end of the file.

See files in test/sql/ for some examples of use.


### [Performances](performances)

To see the real overhead of loading the extension here is some pgbench
in tpcb-like scenario, best of three runs.

* Without loading the extension

```
$ pgbench -h localhost bench -c 20 -j 8 -T 30
starting vacuum...end.
transaction type: <builtin: TPC-B (sort of)>
scaling factor: 1
query mode: simple
number of clients: 20
number of threads: 8
duration: 30 s
number of transactions actually processed: 20454
latency average = 29.366 ms
tps = 681.067975 (including connections establishing)
tps = 681.146941 (excluding connections establishing)
```

* With the use of the extension

```
$ pgbench -h localhost bench -c 20 -j 8 -T 30
starting vacuum...end.
transaction type: <builtin: TPC-B (sort of)>
scaling factor: 1
query mode: simple
number of clients: 20
number of threads: 8
duration: 30 s
number of transactions actually processed: 19716
latency average = 30.464 ms
tps = 656.514964 (including connections establishing)
tps = 656.586600 (excluding connections establishing)
```

### [Authors](#authors)

- Julien Rouhaud
- Dave Sharpe
- Gilles Darold


### [License](#license)

This extension is free software distributed under the PostgreSQL
License.

    Copyright (c) 2020 LzLabs, GmbH

