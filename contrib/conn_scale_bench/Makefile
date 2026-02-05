# contrib/conn_scale_bench/Makefile

PGFILEDESC = "conn_scale_bench - benchmark connection scalability"
PGAPPICON = win32

PROGRAM = conn_scale_bench
OBJS = \
	$(WIN32RES) \
	conn_scale_bench.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/conn_scale_bench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
