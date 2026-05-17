#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 [--cassert|--no-cassert] [run|configure|reconfig|report|clean]

Commands:
  run       Configure if needed, build, run full regress tests, report.
  configure Configure the MC/DC build if it does not already exist.
  reconfig  Remove the MC/DC build/install dirs first, then run.
  report    Reuse existing raw profiles and regenerate reports.
  clean     Remove the MC/DC build/install dirs.

Flags:
  --cassert     Build with cassert=true (overrides MCDC_CASSERT).
  --no-cassert  Build with cassert=false (overrides MCDC_CASSERT).
  -h, --help    Show this help message.

Environment overrides:
  BUILD=/path/to/build-dir
  INSTALL=/path/to/install-dir
  USER_LOCAL_BIN=/path/to/user-local-bin
  RUN_ISOLATION=0
  RUN_INJECTION_ISOLATION=1
  MCDC_CASSERT=true
  SUMMARY_SKIP_EXPANSIONS=0
  REGRESS_SUITE_ARGS="--suite setup --suite regress --num-processes 10"
  REGRESS_TESTS="key_join key_join_matched_filter"  # optional focused override
  COVERAGE_OBJECT=/path/to/one/binary
  COVERAGE_OBJECTS="/path/to/postgres /path/to/other-binary"
  TARGET_SOURCE=/path/to/one/source.c
  TARGET_SOURCES="/path/to/a.c /path/to/b.c"
  ISOLATION_TESTS="key-join-function-race key-join-lazy-facts-lock"
  INJECTION_ISOLATION_TESTS="key_join_proof_race_operator_prelock key_join_proof_race_function_prelock"
EOF
}

log() {
    printf '\n==> %s\n' "$*"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_executable() {
    [[ -x "$1" ]] || die "required executable not found: $1"
}

safe_remove_dir() {
    local dir=$1

    [[ -n "$dir" ]] || die "refusing to remove an empty path"
    [[ "$dir" != "/" ]] || die "refusing to remove /"
    rm -rf "$dir"
}

apply_mcdc_libpq_check_patch() {
    local patch=$MCDC_LIBPQ_CHECK_PATCH
    local target=$SRC/src/interfaces/libpq/libpq_check.pl

    [[ -f "$patch" ]] || die "MC/DC libpq patch not found: $patch"
    [[ -f "$target" ]] || die "libpq_check.pl not found: $target"

    if grep -q "allow_mcdc_profile_atexit" "$target"; then
        log "MC/DC libpq_check.pl coverage patch already applied"
    elif git -C "$SRC" apply --unidiff-zero --check "$patch" >/dev/null 2>&1; then
        log "Applying MC/DC libpq_check.pl coverage patch"
        git -C "$SRC" apply --unidiff-zero "$patch"
    else
        die "MC/DC libpq_check.pl coverage patch does not apply cleanly"
    fi
}

prefer_executable() {
    local preferred=$1
    local fallback=$2

    if [[ -x "$preferred" ]]; then
        printf '%s\n' "$preferred"
    else
        printf '%s\n' "$fallback"
    fi
}

absolute_executable_path() {
    local path=$1
    local dir
    local base

    if [[ "$path" == */* ]]; then
        dir=$(cd "$(dirname "$path")" && pwd -P)
        base=$(basename "$path")
        printf '%s/%s\n' "$dir" "$base"
    else
        command -v "$path"
    fi
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd -P)

SRC=${SRC:-$repo_root}
BUILD=${BUILD:-/Users/joel/build-postgresql-mcdc}
INSTALL=${INSTALL:-/Users/joel/install-postgresql-mcdc}
PROFRAW_DIR=${PROFRAW_DIR:-$BUILD/mcdc-profraw}
REPORT_DIR=${REPORT_DIR:-$BUILD/mcdc-report}
HTML_DIR=$REPORT_DIR/html
PROFDATA=$BUILD/mcdc.profdata
MCDC_LIBPQ_CHECK_PATCH=${MCDC_LIBPQ_CHECK_PATCH:-$script_dir/mcdc_libpq_check_atexit.patch}
DEFAULT_TARGET_SOURCES=(
    "$SRC/src/backend/parser/parse_key_join.c"
    "$SRC/src/backend/catalog/dependency.c"
    "$SRC/src/backend/catalog/pg_proc.c"
    "$SRC/src/backend/commands/functioncmds.c"
    "$SRC/src/backend/commands/keyjoincmds.c"
    "$SRC/src/backend/commands/tablecmds.c"
    "$SRC/src/backend/commands/view.c"
    "$SRC/src/backend/parser/parse_clause.c"
    "$SRC/src/backend/parser/parse_cte.c"
    "$SRC/src/backend/rewrite/rewriteManip.c"
    "$SRC/src/backend/utils/adt/ruleutils.c"
)
POSTGRES_BIN=$BUILD/src/backend/postgres
DEFAULT_COVERAGE_OBJECTS=(
    "$POSTGRES_BIN"
)

BREW_PREFIX=${BREW_PREFIX:-$(brew --prefix)}
LLVM_PREFIX=${LLVM_PREFIX:-$BREW_PREFIX/opt/llvm}
USER_LOCAL_BIN=${USER_LOCAL_BIN:-$HOME/.local/bin}
CLANG=${CLANG:-$LLVM_PREFIX/bin/clang}
CLANGXX=${CLANGXX:-$LLVM_PREFIX/bin/clang++}
LLVM_COV=${LLVM_COV:-$(prefer_executable "$USER_LOCAL_BIN/llvm-cov" "$LLVM_PREFIX/bin/llvm-cov")}
LLVM_PROFDATA=${LLVM_PROFDATA:-$(prefer_executable "$USER_LOCAL_BIN/llvm-profdata" "$LLVM_PREFIX/bin/llvm-profdata")}
BISON=${BISON:-$BREW_PREFIX/opt/bison/bin/bison}
FLEX=${FLEX:-$BREW_PREFIX/opt/flex/bin/flex}
MESON=${MESON:-$BREW_PREFIX/bin/meson}
NINJA=${NINJA:-$BREW_PREFIX/bin/ninja}

OPENLDAP_PKGCONFIG=${OPENLDAP_PKGCONFIG:-$BREW_PREFIX/opt/openldap/lib/pkgconfig}
ICU_PREFIX=${ICU_PREFIX:-$(brew --prefix icu4c 2>/dev/null || true)}

REGRESS_SUITE_ARGS=${REGRESS_SUITE_ARGS:-"--suite setup --suite regress --num-processes 10"}
REGRESS_TESTS=${REGRESS_TESTS:-}
ISOLATION_TESTS=${ISOLATION_TESTS:-"key-join-function-race key-join-lazy-facts-lock"}
INJECTION_ISOLATION_TESTS=${INJECTION_ISOLATION_TESTS:-"key_join_proof_race_operator_prelock key_join_proof_race_function_prelock"}
RUN_ISOLATION=${RUN_ISOLATION:-1}
RUN_INJECTION_ISOLATION=${RUN_INJECTION_ISOLATION:-1}
NINJA_ARGS=${NINJA_ARGS:-}
MESON_TEST_ARGS=${MESON_TEST_ARGS:-"--print-errorlogs"}
MCDC_CASSERT=${MCDC_CASSERT:-true}
SUMMARY_SKIP_EXPANSIONS=${SUMMARY_SKIP_EXPANSIONS:-1}

MCDC_CFLAGS=${MCDC_CFLAGS:-"-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc"}
MCDC_LDFLAGS=${MCDC_LDFLAGS:-"-fprofile-instr-generate"}

target_sources=()
if [[ -n "${TARGET_SOURCES:-}" ]]; then
    read -r -a target_sources <<< "$TARGET_SOURCES"
elif [[ -n "${TARGET_SOURCE:-}" ]]; then
    target_sources=("$TARGET_SOURCE")
else
    target_sources=("${DEFAULT_TARGET_SOURCES[@]}")
fi

coverage_objects=()
if [[ -n "${COVERAGE_OBJECTS:-}" ]]; then
    read -r -a coverage_objects <<< "$COVERAGE_OBJECTS"
elif [[ -n "${COVERAGE_OBJECT:-}" ]]; then
    coverage_objects=("$COVERAGE_OBJECT")
else
    coverage_objects=("${DEFAULT_COVERAGE_OBJECTS[@]}")
fi

source_args=()
for source in "${target_sources[@]}"; do
    source_args+=(--sources "$source")
done

object_args=()
for object in "${coverage_objects[@]:1}"; do
    object_args+=(--object "$object")
done

coverage_summary_args=()
if [[ "$SUMMARY_SKIP_EXPANSIONS" != "0" ]]; then
    coverage_summary_args+=(--summary-skip-expansions)
fi

cassert_override=""
positional=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cassert)
            cassert_override=true
            shift
            ;;
        --no-cassert)
            cassert_override=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            positional+=("$@")
            break
            ;;
        -*)
            die "unknown flag: $1"
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done

set -- "${positional[@]+"${positional[@]}"}"
command=${1:-run}

case "$command" in
    run|configure|reconfig|report|clean)
        ;;
    *)
        usage
        exit 1
        ;;
esac

if [[ -n "$cassert_override" ]]; then
    MCDC_CASSERT=$cassert_override
fi

require_executable "$CLANG"
require_executable "$CLANGXX"
require_executable "$LLVM_COV"
require_executable "$LLVM_PROFDATA"
require_executable "$BISON"
require_executable "$FLEX"
require_executable "$MESON"
require_executable "$NINJA"

log "Using LLVM binaries"
printf 'clang:          %s\n' "$(absolute_executable_path "$CLANG")"
printf 'clang++:        %s\n' "$(absolute_executable_path "$CLANGXX")"
printf 'llvm-cov:      %s\n' "$(absolute_executable_path "$LLVM_COV")"
printf 'llvm-profdata: %s\n' "$(absolute_executable_path "$LLVM_PROFDATA")"
if ((${#coverage_summary_args[@]})); then
    printf 'summary args:   %s\n' "${coverage_summary_args[*]}"
else
    printf 'summary args:   (none)\n'
fi
printf 'coverage objects:\n'
printf '  %s\n' "${coverage_objects[@]}"
printf 'target sources:\n'
printf '  %s\n' "${target_sources[@]}"

if [[ "$command" == "clean" || "$command" == "reconfig" ]]; then
    log "Removing MC/DC build and install directories"
    safe_remove_dir "$BUILD"
    safe_remove_dir "$INSTALL"
    [[ "$command" == "clean" ]] && exit 0
fi

if [[ "$command" != "report" ]]; then
    apply_mcdc_libpq_check_patch
fi

configure_build() {
    local pkg_config_path_parts=()
    local pkg_config_path_value
    local setup_profraw_dir=$BUILD/meson-setup-profraw

    [[ -d "$OPENLDAP_PKGCONFIG" ]] && pkg_config_path_parts+=("$OPENLDAP_PKGCONFIG")
    [[ -n "$ICU_PREFIX" && -d "$ICU_PREFIX/lib/pkgconfig" ]] && pkg_config_path_parts+=("$ICU_PREFIX/lib/pkgconfig")
    [[ -n "${PKG_CONFIG_PATH:-}" ]] && pkg_config_path_parts+=("$PKG_CONFIG_PATH")
    pkg_config_path_value=$(IFS=:; echo "${pkg_config_path_parts[*]}")

    log "Configuring MC/DC build"
    printf 'Source:  %s\n' "$SRC"
    printf 'Build:   %s\n' "$BUILD"
    printf 'Install: %s\n' "$INSTALL"
    printf 'Clang:   %s\n' "$("$CLANG" --version | head -1)"

    mkdir -p "$BUILD" "$INSTALL" "$setup_profraw_dir"

    env \
        CC="$CLANG" \
        CXX="$CLANGXX" \
        LLVM_PROFILE_FILE="$setup_profraw_dir/meson-%p.profraw" \
        PKG_CONFIG_PATH="$pkg_config_path_value" \
        "$MESON" setup "$BUILD" "$SRC" \
            --prefix="$INSTALL" \
            --buildtype=debug \
            -Dc_args="$MCDC_CFLAGS ${CFLAGS:-}" \
            -Dc_link_args="$MCDC_LDFLAGS ${LDFLAGS:-}" \
            -Db_coverage=false \
            -Dcassert="$MCDC_CASSERT" \
            -Dinjection_points=true \
            -Dicu=enabled \
            -DBISON="$BISON" \
            -DFLEX="$FLEX"
}

ensure_build_options() {
    local setup_profraw_dir=$BUILD/meson-setup-profraw

    mkdir -p "$setup_profraw_dir"
    log "Ensuring MC/DC Meson options"
    env LLVM_PROFILE_FILE="$setup_profraw_dir/meson-%p.profraw" \
        "$MESON" configure "$BUILD" \
        -Dc_args="$MCDC_CFLAGS ${CFLAGS:-}" \
        -Dc_link_args="$MCDC_LDFLAGS ${LDFLAGS:-}" \
        -Db_coverage=false \
        -Dcassert="$MCDC_CASSERT" \
        -Dinjection_points=true \
        -Dicu=enabled \
        -DBISON="$BISON" \
        -DFLEX="$FLEX"
}

build_tree() {
    local build_log=$BUILD/mcdc-ninja.log

    log "Building instrumented PostgreSQL"
    # shellcheck disable=SC2086
    if ! env POSTGRES_MCDC_ALLOW_ATEEXIT=1 \
        "$NINJA" -C "$BUILD" $NINJA_ARGS >"$build_log" 2>&1; then
        cat "$build_log" >&2
        die "ninja build failed; see $build_log"
    fi

    log "Installing instrumented PostgreSQL"
    mkdir -p "$PROFRAW_DIR"
    env LLVM_PROFILE_FILE="$PROFRAW_DIR/install-%16m.profraw" \
        "$MESON" install -C "$BUILD" --quiet --no-rebuild
}

run_tests() {
    mkdir -p "$PROFRAW_DIR"
    rm -f "$PROFRAW_DIR"/*.profraw

    if [[ -n "$REGRESS_TESTS" ]]; then
        log "Running setup tests with profile capture"
        env LLVM_PROFILE_FILE="$PROFRAW_DIR/setup-%16m.profraw" \
            "$MESON" test -C "$BUILD" $MESON_TEST_ARGS --suite setup

        log "Running focused regression tests"
        printf 'TESTS=%s\n' "$REGRESS_TESTS"
        env LLVM_PROFILE_FILE="$PROFRAW_DIR/regress-%16m.profraw" \
            TESTS="$REGRESS_TESTS" \
            "$MESON" test -C "$BUILD" $MESON_TEST_ARGS postgresql:regress/regress
    else
        log "Running full setup and regression suites with profile capture"
        printf 'REGRESS_SUITE_ARGS=%s\n' "$REGRESS_SUITE_ARGS"
        env LLVM_PROFILE_FILE="$PROFRAW_DIR/regress-%16m.profraw" \
            "$MESON" test -C "$BUILD" $MESON_TEST_ARGS $REGRESS_SUITE_ARGS
    fi

    if [[ "$RUN_ISOLATION" != "0" ]]; then
        log "Running focused isolation tests"
        printf 'TESTS=%s\n' "$ISOLATION_TESTS"
        env LLVM_PROFILE_FILE="$PROFRAW_DIR/isolation-%16m.profraw" \
            TESTS="$ISOLATION_TESTS" \
            "$MESON" test -C "$BUILD" $MESON_TEST_ARGS postgresql:isolation/isolation
    fi

    if [[ "$RUN_INJECTION_ISOLATION" != "0" ]]; then
        log "Running focused injection-points isolation tests"
        printf 'TESTS=%s\n' "$INJECTION_ISOLATION_TESTS"
        env LLVM_PROFILE_FILE="$PROFRAW_DIR/injection-isolation-%16m.profraw" \
            TESTS="$INJECTION_ISOLATION_TESTS" \
            "$MESON" test -C "$BUILD" $MESON_TEST_ARGS postgresql:injection_points/isolation
    fi
}

merge_profiles() {
    local profiles=("$PROFRAW_DIR"/*.profraw)

    [[ -e "${profiles[0]}" ]] || die "no .profraw files found in $PROFRAW_DIR"

    log "Merging ${#profiles[@]} raw profile files"
    "$LLVM_PROFDATA" merge -sparse "${profiles[@]}" -o "$PROFDATA"
}

generate_reports() {
    local summary_report=$REPORT_DIR/parse_key_join_mcdc_summary.txt
    local detail_report=$REPORT_DIR/parse_key_join_mcdc_detail.txt
    local primary_object=${coverage_objects[0]}

    [[ -n "$primary_object" ]] || die "no coverage object configured"
    for object in "${coverage_objects[@]}"; do
        [[ -e "$object" ]] || die "coverage object not found: $object"
    done
    [[ -f "$PROFDATA" ]] || die "merged profile not found: $PROFDATA"
    for source in "${target_sources[@]}"; do
        [[ -f "$source" ]] || die "target source not found: $source"
    done

    mkdir -p "$REPORT_DIR"
    rm -rf "$HTML_DIR"

    log "Writing text summary"
    "$LLVM_COV" report "$primary_object" \
        -instr-profile="$PROFDATA" \
        ${object_args[@]+"${object_args[@]}"} \
        --show-branch-summary \
        --show-mcdc-summary \
        "${coverage_summary_args[@]}" \
        "${source_args[@]}" | tee "$summary_report"

    log "Writing detailed MC/DC text report"
    "$LLVM_COV" show "$primary_object" \
        -instr-profile="$PROFDATA" \
        ${object_args[@]+"${object_args[@]}"} \
        --show-branches=count \
        --show-mcdc \
        --show-mcdc-summary \
        "${coverage_summary_args[@]}" \
        "${source_args[@]}" > "$detail_report"

    log "Writing HTML report"
    "$LLVM_COV" show "$primary_object" \
        -instr-profile="$PROFDATA" \
        ${object_args[@]+"${object_args[@]}"} \
        --show-branches=count \
        --show-mcdc \
        --show-mcdc-summary \
        "${coverage_summary_args[@]}" \
        --format=html \
        --output-dir="$HTML_DIR" \
        "${source_args[@]}"

    cat <<EOF

MC/DC reports written:
  Summary: $summary_report
  Detail:  $detail_report
  HTML:    $HTML_DIR/index.html
EOF
}

if [[ "$command" == "configure" ]]; then
    if [[ ! -f "$BUILD/build.ninja" ]]; then
        configure_build
    else
        log "MC/DC build already configured: $BUILD"
        ensure_build_options
    fi
    exit 0
fi

if [[ "$command" != "report" ]]; then
    if [[ ! -f "$BUILD/build.ninja" ]]; then
        configure_build
    else
        log "Reusing existing MC/DC build: $BUILD"
        ensure_build_options
    fi

    build_tree
    run_tests
fi

merge_profiles
generate_reports
