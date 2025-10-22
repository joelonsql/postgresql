# src/test/modules/test_listen_notify/Makefile

MODULE = test_listen_notify
PGFILEDESC = "test_listen_notify - regression testing for LISTEN/NOTIFY support"

TAP_TESTS = 1

EXTRA_INSTALL=src/test/modules/xid_wraparound

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_listen_notify
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
