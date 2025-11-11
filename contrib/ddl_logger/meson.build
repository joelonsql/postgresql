# Copyright (c) 2022-2025, PostgreSQL Global Development Group

ddl_logger_sources = files(
  'ddl_logger.c',
)

if host_system == 'windows'
  ddl_logger_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ddl_logger',
    '--FILEDESC', 'ddl_logger - logs DDL commands to files by transaction ID',])
endif

ddl_logger = shared_module('ddl_logger',
  ddl_logger_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += ddl_logger
