# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'logtape.c',
  'qsort_interruptible.c',
  'sharedtuplestore.c',
  'sortsupport.c',
  'tuplesort.c',
  'tuplesortvariants.c',
  'tuplestore.c',
)

# Add GPU support on macOS
if host_system == 'darwin'
  # Compile GPU file as Objective-C with explicit clang command
  tuplesort_gpu_direct_obj = custom_target('tuplesort_gpu_direct.o',
    input: 'tuplesort_gpu_direct.m',
    output: 'tuplesort_gpu_direct.o',
    command: [
      'clang', '-x', 'objective-c', '-c', '@INPUT@', '-o', '@OUTPUT@',
      '-I' + meson.project_source_root() + '/src/include',
      '-framework', 'Metal', '-framework', 'Foundation',
      '-fPIC', '-O2', '-std=c11'
    ],
    build_by_default: true,
  )
  backend_sources += tuplesort_gpu_direct_obj
endif
