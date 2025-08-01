# PostgreSQL Build and Test Guide for AI Agents

This document provides instructions for AI coding assistants on how to build and test PostgreSQL.

## Quick Start

PostgreSQL uses Meson as its build system. Here's the minimal setup:

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y build-essential meson ninja-build \
  libreadline-dev zlib1g-dev libssl-dev

# Configure
meson setup build

# Build
ninja -C build

# Run tests
meson test -C build
```

## Detailed Build Instructions

### Prerequisites

1. **Build Tools**
   - C compiler (GCC or Clang)
   - Meson (>= 0.57.2)
   - Ninja build system
   - Python 3

2. **Required Libraries**
   - readline (for psql)
   - zlib (for compression)
   - OpenSSL (for encryption)

3. **Optional Libraries**
   - libxml2, libxslt (for XML support)
   - liblz4, libzstd (for compression)
   - libldap (for LDAP auth)
   - libpam (for PAM auth)
   - libsystemd (for systemd integration)
   - liburing (for io_uring support)

### Build Commands

```bash
# Full featured build with debug symbols
meson setup \
  --buildtype=debug \
  -Dcassert=true \
  -Dtap_tests=enabled \
  build

# Production build
meson setup \
  --buildtype=release \
  --prefix=/usr/local/pgsql \
  build

# Build with specific number of jobs
ninja -C build -j4

# Install (after building)
ninja -C build install
```

### Testing

```bash
# Run all tests
meson test -C build

# Run specific test suite
meson test -C build --suite setup
meson test -C build --suite regress

# Run with more details on failure
meson test -C build --print-errorlogs

# Run tests in parallel (8 jobs)
meson test -C build --num-processes 8
```

## Common Tasks

### Clean Build

```bash
# Remove build directory
rm -rf build

# Or use ninja
ninja -C build clean
```

### Reconfigure

```bash
# Change configuration options
meson configure build -Dssl=openssl

# See all options
meson configure build
```

### Development Workflow

```bash
# Make changes to source files
vim src/backend/...

# Rebuild (incremental)
ninja -C build

# Run specific test
meson test -C build specific_test_name

# Check for compilation issues
ninja -C build -t missingdeps
```

## CI/CD Integration

### GitHub Actions

A workflow is provided in `.github/workflows/ci.yml` that:
1. Installs all dependencies
2. Configures with meson
3. Builds PostgreSQL
4. Runs the test suite
5. Uploads test results on failure

### Running CI Locally

```bash
# Simulate CI environment
export MTEST_ARGS="--print-errorlogs --no-rebuild -C build"
export BUILD_JOBS=4
export TEST_JOBS=8

meson setup --buildtype=debug -Dcassert=true build
ninja -C build -j${BUILD_JOBS} all testprep
meson test $MTEST_ARGS --num-processes ${TEST_JOBS}
```

## Troubleshooting

### Build Failures

1. **Missing dependencies**: Check error messages and install required `-dev` packages
2. **Old build artifacts**: Remove `build/` directory and reconfigure
3. **Permission issues**: Ensure you have write access to the source directory

### Test Failures

1. **Check logs**: `build/testrun/*/log`
2. **Regression diffs**: `build/testrun/*/regression.diffs`
3. **Disable problematic tests**: Set `PG_TEST_EXTRA` to exclude specific test types

### Common Issues

- **Locale problems**: Set `LANG=C` for consistent test results
- **Port conflicts**: Ensure ports 5432+ are available for parallel tests
- **Memory limits**: Increase ulimits if tests fail with resource errors

## Important Files

- `meson.build` - Main build configuration
- `src/` - Source code
- `contrib/` - Additional modules
- `doc/` - Documentation
- `.cirrus.tasks.yml` - CI configuration reference

## Additional Resources

- [PostgreSQL Documentation](https://www.postgresql.org/docs/)
- [Meson Documentation](https://mesonbuild.com/)
- [PostgreSQL Wiki - Developer FAQ](https://wiki.postgresql.org/wiki/Developer_FAQ)