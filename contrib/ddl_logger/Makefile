# contrib/ddl_logger/Makefile

MODULE_big = ddl_logger
OBJS = \
	$(WIN32RES) \
	ddl_logger.o
PGFILEDESC = "ddl_logger - logs DDL commands to files by transaction ID"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ddl_logger
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
