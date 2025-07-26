#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/sort
#
# IDENTIFICATION
#    src/backend/utils/sort/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/sort
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	logtape.o \
	qsort_interruptible.o \
	sharedtuplestore.o \
	sortsupport.o \
	tuplesort.o \
	tuplesortvariants.o \
	tuplestore.o

# Add GPU support on macOS
ifeq ($(shell uname), Darwin)
OBJS += tuplesort_gpu_direct.o
endif

include $(top_srcdir)/src/backend/common.mk
