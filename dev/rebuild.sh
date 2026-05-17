#!/bin/bash
set -e

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd -P)

SRC=${SRC:-$repo_root}
BUILD=/Users/joel/build-postgresql-release
INSTALL=/Users/joel/install-postgresql-release
COVERAGE_BUILD=/Users/joel/build-postgresql-coverage
COVERAGE_INSTALL=/Users/joel/install-postgresql-coverage
DATA=/Users/joel/pg-data
OPENLDAP_PKGCONFIG=/opt/homebrew/opt/openldap/lib/pkgconfig
ICU_PREFIX=$(brew --prefix icu4c 2>/dev/null || true)

PKG_CONFIG_PATHS=()
[[ -d "$OPENLDAP_PKGCONFIG" ]] && PKG_CONFIG_PATHS+=("$OPENLDAP_PKGCONFIG")
[[ -n "$ICU_PREFIX" && -d "$ICU_PREFIX/lib/pkgconfig" ]] && PKG_CONFIG_PATHS+=("$ICU_PREFIX/lib/pkgconfig")
[[ -n "$PKG_CONFIG_PATH" ]] && PKG_CONFIG_PATHS+=("$PKG_CONFIG_PATH")
export PKG_CONFIG_PATH=$(IFS=:; echo "${PKG_CONFIG_PATHS[*]}")

CMD=${1:-}

usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  (none)    Compile, install, and run tests"
    echo "  reinit    Also reinitialize the data directory"
    echo "  reconfig  Also reconfigure the build (implies reinit)"
    echo "  test      Also run tests"
    echo "  coverage-html  Build with normal coverage and generate HTML report"
    exit 1
}

if [[ "$CMD" != "" && "$CMD" != "reinit" && "$CMD" != "reconfig" && "$CMD" != "test" && "$CMD" != "coverage-html" ]]; then
    usage
fi

run_coverage_html() {
    local python_user_base
    local coverage_report=$COVERAGE_BUILD/meson-logs/coveragereport/index.html

    python_user_base=$(python3 -m site --user-base 2>/dev/null || true)
    if [[ -n "$python_user_base" && -x "$python_user_base/bin/gcovr" ]]; then
        export PATH="$python_user_base/bin:$PATH"
    fi

    if ! command -v gcovr >/dev/null; then
        echo "error: gcovr is required for meson coverage-html" >&2
        echo "hint: python3 -m pip install --user gcovr" >&2
        exit 1
    fi
    if ! command -v gcov >/dev/null; then
        echo "error: gcov is required for coverage-html" >&2
        exit 1
    fi

    if [[ ! -f "$COVERAGE_BUILD/build.ninja" ]]; then
        echo "Configuring coverage build with meson..."
        meson setup "$COVERAGE_BUILD" "$SRC" \
            --prefix="$COVERAGE_INSTALL" \
            --buildtype=debug \
            -Dcassert=true \
            -Dicu=enabled \
            -Db_coverage=true
    else
        echo "Ensuring coverage build options..."
        meson configure "$COVERAGE_BUILD" \
            -Dcassert=true \
            -Dicu=enabled \
            -Db_coverage=true
    fi

    echo "Compiling coverage build..."
    ninja -C "$COVERAGE_BUILD" >/dev/null

    echo "Installing coverage build..."
    ninja -C "$COVERAGE_BUILD" install >/dev/null

    echo "Clearing old coverage counters..."
    find "$COVERAGE_BUILD" -name '*.gcda' -delete
    find "$COVERAGE_BUILD" -name '*.gcov' -delete
    rm -rf "$COVERAGE_BUILD/meson-logs/coveragereport"
    mkdir -p "$COVERAGE_BUILD/meson-logs/coveragereport"

    echo "Running coverage tests..."
    meson test -C "$COVERAGE_BUILD" --print-errorlogs --suite setup --suite regress --num-processes 10 &>"$COVERAGE_BUILD/coverage-test-output.log"

    echo "Generating HTML coverage report..."
    gcovr -r "$SRC" "$COVERAGE_BUILD" \
        -e "$SRC/subprojects" \
        --gcov-executable "$(command -v gcov)" \
        --gcov-suspicious-hits-threshold=0 \
        --merge-mode-functions=merge-use-line-min \
        --html \
        --html-nested \
        --print-summary \
        -o "$coverage_report"

    echo "Coverage report: $coverage_report"
    echo "Test output: $COVERAGE_BUILD/coverage-test-output.log"
}

if [[ "$CMD" == "coverage-html" ]]; then
    run_coverage_html
    exit 0
fi

echo "Stopping server..."
$INSTALL/bin/pg_ctl -D $DATA stop &>/dev/null || true

if [[ "$CMD" == "reconfig" ]]; then
    echo "Cleaning up directories..."
    rm -rf $DATA
    rm -rf $BUILD
    rm -rf $INSTALL

    echo "Configuring build with meson..."
    meson setup $BUILD $SRC --prefix=$INSTALL --buildtype=debug -Dcassert=true -Dicu=enabled -Dinjection_points=true
fi

echo "Compiling..."
ninja -C $BUILD >/dev/null

echo "Installing..."
ninja -C $BUILD install >/dev/null

if [[ "$CMD" == "test" ]]; then
    echo "Running tests..."
    meson test -C $BUILD --print-errorlogs \
        --suite setup \
        --suite regress \
        --suite isolation \
        --suite injection_points \
        --num-processes 10 &>$BUILD/test-output.log
fi

# Uncomment the below (and comment the above) to instead run the full test suite:
# meson test -C $BUILD --print-errorlogs -q --num-processes 10 &>$BUILD/test-output.log

if [[ "$CMD" == "reinit" || "$CMD" == "reconfig" ]]; then
    echo "Removing old data directory..."
    rm -rf $DATA
fi

if [[ ! -d $DATA ]]; then
    echo "Initializing database..."
    $INSTALL/bin/initdb -D $DATA &>/dev/null
    NEED_CREATEDB=1

    echo "Configuring pg_hba.conf for Secure Enclave client cert authentication..."
    cat > $DATA/pg_hba.conf <<EOF
# TYPE  DATABASE        USER            ADDRESS                 METHOD
local   all             all                                     trust
# Require client certificate for SSL connections (tests Secure Enclave)
hostssl all             all             127.0.0.1/32            trust
hostssl all             all             ::1/128                 trust
hostssl all             all             0.0.0.0/0               trust
hostssl all             all             ::/0                    trust
EOF
fi

if [[ -n "$NEED_CREATEDB" ]]; then
    echo "Setting up SSL certificates..."
    # Generate self-signed server certificate and key
    openssl req -new -x509 -days 365 -nodes \
        -out $DATA/server.crt \
        -keyout $DATA/server.key \
        -subj "/CN=localhost"

    # Set proper permissions on the private key (required by PostgreSQL)
    chmod 600 $DATA/server.key

    echo "Setting up Secure Enclave CA and client certificates..."

    # Enable SSL via postgresql.auto.conf (equivalent to ALTER SYSTEM)
    cat >> $DATA/postgresql.auto.conf <<EOF
ssl = on
ssl_cert_file = 'server.crt'
ssl_key_file = 'server.key'
EOF
fi

echo "Starting server..."
$INSTALL/bin/pg_ctl -D $DATA -l $DATA/logfile start >/dev/null

if [[ -n "$NEED_CREATEDB" ]]; then
    echo "Creating user database..."
    # Use local socket connection (trust auth) instead of TCP (cert auth)
    $INSTALL/bin/createdb $USER >/dev/null
fi

echo "Done!"
echo "Test output: $BUILD/test-output.log"

echo "export PATH=\"$INSTALL/bin:\$PATH\""
