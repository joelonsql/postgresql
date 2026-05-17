#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd -P)

SRC=${SRC:-$repo_root}
BUILD=${BUILD:-/Users/joel/build-postgresql-coverage}
INSTALL=${INSTALL:-/Users/joel/install-postgresql-coverage}
DATA=${DATA:-/Users/joel/pg-data-coverage-probe}
STATE=${STATE:-$BUILD/parse-key-join-probe}
PORT=${PORT:-55432}
DB=${DB:-$USER}
PGUSER=${PGUSER:-$USER}

SOURCE=$SRC/src/backend/parser/parse_key_join.c
OBJDIR=$BUILD/src/backend/postgres_lib.a.p
GCDA=$OBJDIR/parser_parse_key_join.c.gcda
GCNO=$OBJDIR/parser_parse_key_join.c.gcno
REPORT_DIR=$BUILD/meson-logs/coveragereport

BASELINE=$STATE/baseline.tsv
LAST=$STATE/last.tsv
CURRENT=$STATE/current.tsv
SOCKDIR=$STATE/socket
GCOV_WORK=$STATE/gcov-work

PSQL=$INSTALL/bin/psql
PG_CTL=$INSTALL/bin/pg_ctl
INITDB=$INSTALL/bin/initdb
CREATEDB=$INSTALL/bin/createdb

usage() {
	cat <<EOF
Usage:
  $0 start
  $0 restart
  $0 stop
  $0 status
  $0 baseline [--from-report|--from-gcda]
  $0 run -c 'SQL'
  $0 run FILE.sql

Environment:
  SRC=$SRC
  BUILD=$BUILD
  INSTALL=$INSTALL
  DATA=$DATA
  STATE=$STATE
  PORT=$PORT
  DB=$DB

The probe deletes only:
  $GCDA

Each run uses a fresh psql connection so the backend exits and writes gcda.
The output is a raw per-line count diff against the saved baseline and the
previous probe.

This script is scoped to src/backend/parser/parse_key_join.c, but not to any
particular coverage case.  Override STATE to reuse snapshots from an older
probe directory.
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

ensure_paths() {
	[[ -f "$SOURCE" ]] || die "missing source: $SOURCE"
	[[ -f "$GCNO" ]] || die "missing gcno: $GCNO"
	[[ -x "$PSQL" ]] || die "missing psql: $PSQL"
	mkdir -p "$STATE" "$SOCKDIR"
}

server_running() {
	[[ -d "$DATA" ]] && "$PG_CTL" -D "$DATA" status >/dev/null 2>&1
}

# True when the on-disk gcno was regenerated after we last started the server.
# Detects `dev/rebuild.sh coverage-html` (or any other recompile) having shifted
# the source-line checksums out from under the running postmaster — gcov would
# otherwise error with "checksums do not match" the next time we read gcda.
needs_restart() {
	local marker=$STATE/server_started
	[[ -f "$marker" ]] || return 1
	local marker_mtime gcno_mtime
	marker_mtime=$(stat -f %m "$marker" 2>/dev/null || echo 0)
	gcno_mtime=$(stat -f %m "$GCNO" 2>/dev/null || echo 0)
	(( gcno_mtime > marker_mtime ))
}

start_server() {
	ensure_paths

	if server_running && needs_restart; then
		echo "gcno newer than server (coverage rebuild detected); restarting..."
		"$PG_CTL" -D "$DATA" stop >/dev/null
	fi

	if ! server_running; then
		ninja -C "$BUILD" install >/dev/null
	fi

	if [[ ! -d "$DATA" ]]; then
		"$INITDB" -D "$DATA" -A trust -U "$PGUSER" >/dev/null
	fi

	# postgresql.probe.conf is rewritten on every start so renaming STATE
	# (and thus SOCKDIR) doesn't strand an existing data directory pointing
	# at a now-missing socket path.  PostgreSQL's "last value wins" rule
	# means this include — appended at the very end — overrides any legacy
	# settings appended by an older version of this script.
	if ! grep -q "^include = 'postgresql.probe.conf'" "$DATA/postgresql.conf"; then
		printf '\n%s\n%s\n' \
			"# managed by parse_key_join_probe.sh" \
			"include = 'postgresql.probe.conf'" \
			>>"$DATA/postgresql.conf"
	fi
	cat >"$DATA/postgresql.probe.conf" <<EOF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$SOCKDIR'
fsync = off
EOF

	if ! server_running; then
		"$PG_CTL" -D "$DATA" -l "$DATA/logfile" start >/dev/null
		touch "$STATE/server_started"
	fi

	"$CREATEDB" -h "$SOCKDIR" -p "$PORT" -U "$PGUSER" "$DB" >/dev/null 2>&1 || true
	echo "coverage server ready"
	echo "psql: $PSQL -h $SOCKDIR -p $PORT -U $PGUSER $DB"
}

restart_server() {
	stop_server
	start_server
}

stop_server() {
	if [[ -d "$DATA" ]]; then
		"$PG_CTL" -D "$DATA" stop >/dev/null 2>&1 || true
	fi
}

status_server() {
	if server_running; then
		echo "running"
	else
		echo "stopped"
	fi
}

report_path() {
	local report

	report=$(ls -t "$REPORT_DIR"/index.parse_key_join.c.*.html 2>/dev/null | head -1 || true)
	[[ -n "$report" ]] || die "no parse_key_join.c HTML report under $REPORT_DIR"
	printf '%s\n' "$report"
}

snapshot_from_report() {
	local out=$1
	local report=$2

	python3 - "$SOURCE" "$report" "$out" <<'PY'
import html
import re
import sys

source_path, report_path, out_path = sys.argv[1:]
source = [""] + [line.rstrip("\n") for line in open(source_path)]
counts = {}
report_source = {}
current_line = None

line_re = re.compile(r'<a id="l([0-9]+)"')
count_re = re.compile(r'<td class="linecount[^"]*">(.*?)</td>')
src_re = re.compile(r'<td class="src[^"]*">(.*?)</td>')

for raw in open(report_path, encoding="utf-8"):
    m = line_re.search(raw)
    if m:
        current_line = int(m.group(1))
        continue
    if current_line is None:
        continue
    m = count_re.search(raw)
    if m:
        text = re.sub(r"<[^>]+>", "", m.group(1)).strip()
        text = html.unescape(text)
        if text == "" or text == "-":
            counts[current_line] = ""
        elif text in {"×", "✗"} or "cross" in text:
            counts[current_line] = "0"
        else:
            text = text.replace(",", "")
            counts[current_line] = str(int(text)) if text.isdigit() else ""
        continue
    m = src_re.search(raw)
    if m:
        text = html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).rstrip()
        report_source[current_line] = text
        current_line = None

mismatches = []
for lineno, text in report_source.items():
    if 0 < lineno < len(source) and source[lineno] != text:
        mismatches.append(lineno)
if mismatches:
    print(
        "warning: HTML report source differs from current source at "
        f"{len(mismatches)} line(s); baseline may be line-shifted",
        file=sys.stderr,
    )
    sample = ", ".join(str(x) for x in mismatches[:12])
    print(f"warning: first mismatched lines: {sample}", file=sys.stderr)

with open(out_path, "w", encoding="utf-8") as out:
    for lineno in range(1, len(source)):
        out.write(f"{lineno}\t{counts.get(lineno, '')}\t{source[lineno]}\n")
PY
}

render_gcov() {
	rm -rf "$GCOV_WORK"
	mkdir -p "$GCOV_WORK" "$STATE/src"
	ln -sfn "$SRC" "$STATE/src/postgresql"

	(
		cd "$GCOV_WORK"
		gcov -b -c "$GCDA" >"$STATE/gcov.out" 2>"$STATE/gcov.err"
	)

	[[ -f "$GCOV_WORK/parse_key_join.c.gcov" ]] ||
		die "gcov did not create parse_key_join.c.gcov; see $STATE/gcov.err"
	if rg -q "Invalid .gcda File|checksums do not match" "$STATE/gcov.err"; then
		cat "$STATE/gcov.err" >&2
		die "gcda/gcno mismatch; delete gcda and run a fresh SQL probe"
	fi
}

snapshot_from_gcov() {
	local out=$1

	[[ -f "$GCDA" ]] || die "missing gcda: $GCDA"
	render_gcov
	python3 - "$SOURCE" "$GCOV_WORK/parse_key_join.c.gcov" "$out" <<'PY'
import re
import sys

source_path, gcov_path, out_path = sys.argv[1:]
source = [""] + [line.rstrip("\n") for line in open(source_path)]
counts = {}
row_re = re.compile(r"^\s*([^:]+):\s*([0-9]+):(.*)$")

for raw in open(gcov_path, encoding="utf-8", errors="replace"):
    m = row_re.match(raw.rstrip("\n"))
    if not m:
        continue
    raw_count, raw_line, _ = m.groups()
    lineno = int(raw_line)
    raw_count = raw_count.strip()
    if raw_count == "-":
        counts[lineno] = ""
    elif raw_count.startswith("#") or raw_count.startswith("="):
        counts[lineno] = "0"
    else:
        raw_count = raw_count.rstrip("*").replace(",", "")
        counts[lineno] = str(int(raw_count)) if raw_count.isdigit() else ""

with open(out_path, "w", encoding="utf-8") as out:
    for lineno in range(1, len(source)):
        out.write(f"{lineno}\t{counts.get(lineno, '')}\t{source[lineno]}\n")
PY
}

compare_snapshots() {
	local current=$1
	local baseline=${2:-}
	local previous=${3:-}

	python3 - "$SOURCE" "$current" "$baseline" "$previous" <<'PY'
import sys

source_path, current_path, baseline_path, previous_path = sys.argv[1:]
source = [""] + [line.rstrip("\n") for line in open(source_path)]

def load(path):
    if not path:
        return {}
    try:
        rows = {}
        for raw in open(path, encoding="utf-8"):
            lineno, count, text = raw.rstrip("\n").split("\t", 2)
            rows[int(lineno)] = (None if count == "" else int(count), text)
        return rows
    except FileNotFoundError:
        return {}

def covered(count):
    return count is not None and count > 0

def count_str(count):
    return "-" if count is None else str(count)

current = load(current_path)
baseline = load(baseline_path)
previous = load(previous_path)

def line_text(lineno):
    return source[lineno] if 0 < lineno < len(source) else current.get(lineno, ("", ""))[1]

def diff_sign(old, new):
    if covered(new) and not covered(old):
        return "+"
    if covered(old) and not covered(new):
        return "-"
    return "~"

def changed_rows(left, right):
    rows = []
    for lineno in sorted(set(left) | set(right)):
        old = left.get(lineno, (None, ""))[0]
        new = right.get(lineno, (None, ""))[0]
        if old != new:
            rows.append((lineno, old, new))
    return rows

def print_rows(title, rows):
    print(title)
    if not rows:
        print("  none")
        return
    for lineno, old, new in rows:
        sign = diff_sign(old, new)
        print(f"  {sign}{lineno} {count_str(old)} -> {count_str(new)}  {line_text(lineno).strip()}")

if previous:
    print_rows("Changed vs previous probe:", changed_rows(previous, current))
else:
    print("Changed vs previous probe:")
    print("  no previous probe snapshot")

if baseline:
    print_rows("Changed vs baseline:", changed_rows(baseline, current))
else:
    print("Changed vs baseline:")
    print("  no baseline snapshot")
PY
}

cmd_baseline() {
	ensure_paths
	local mode=${1:---from-report}

	case "$mode" in
		--from-report)
			local report
			report=$(report_path)
			snapshot_from_report "$BASELINE" "$report"
			echo "baseline saved from report: $report"
			;;
		--from-gcda)
			snapshot_from_gcov "$BASELINE"
			echo "baseline saved from gcda: $GCDA"
			;;
		*)
			die "unknown baseline mode: $mode"
			;;
	esac
	rm -f "$LAST" "$CURRENT"
}

run_psql_probe() {
	ensure_paths
	start_server >/dev/null

	rm -f "$GCDA"
	local sql_rc

	if [[ ${1:-} == "-c" ]]; then
		[[ $# -ge 2 ]] || die "run -c requires SQL text"
		set +e
		"$PSQL" -h "$SOCKDIR" -p "$PORT" -U "$PGUSER" -d "$DB" \
			-v ON_ERROR_STOP=1 -X -q -c "$2"
		sql_rc=$?
		set -e
	else
		[[ $# -ge 1 ]] || die "run requires -c SQL or FILE.sql"
		[[ -f "$1" ]] || die "missing SQL file: $1"
		set +e
		"$PSQL" -h "$SOCKDIR" -p "$PORT" -U "$PGUSER" -d "$DB" \
			-v ON_ERROR_STOP=1 -X -q -f "$1"
		sql_rc=$?
		set -e
	fi

	[[ -f "$GCDA" ]] || die "probe did not write gcda: $GCDA"
	snapshot_from_gcov "$CURRENT"
	if [[ $sql_rc -eq 0 ]]; then
		echo "SQL: ok"
	else
		echo "SQL: error (psql exit $sql_rc)"
	fi
	echo "gcda: $GCDA"
	compare_snapshots "$CURRENT" "$BASELINE" "$LAST"
	cp "$CURRENT" "$LAST"
}

main() {
	local cmd=${1:-}
	shift || true

	case "$cmd" in
		start) start_server ;;
		restart) restart_server ;;
		stop) stop_server ;;
		status) status_server ;;
		baseline) cmd_baseline "$@" ;;
		run) run_psql_probe "$@" ;;
		""|-h|--help|help) usage ;;
		*) usage; exit 1 ;;
	esac
}

main "$@"
