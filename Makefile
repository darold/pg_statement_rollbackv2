LIB = pg_statement_rollback

PGFILEDESC = "pg_statement_rollback - Automatic rollback at statement level for PostgreSQL"

PG_CONFIG = /usr/local/pgsql/bin/pg_config

# Test that we can install a bgworker, PG >= 9.4
PG94 = $(shell $(PG_CONFIG) --version | egrep " 8\.| 9\.[0-3]" > /dev/null && echo no || echo yes)
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LDFLAGS = -L$(libpq_builddir) -lpq

SHLIB_LINK = $(libpq)

ifeq ($(PG94),yes)

DOCS = $(wildcard README*)
MODULES = pg_statement_rollback

TESTS        = 00_slr_simple \
	       01_slr_trigger  \
	       02_slr_release \
	       03_slr_cursor \
	       04_slr_write_cte \
	       05_slr_do_block

REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test

else
	$(error Minimum version of PostgreSQL required is 9.4.0)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

