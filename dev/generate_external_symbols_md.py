#!/usr/bin/env python3
#
# Generate the external-symbols spec markdown by parsing a configurable set
# of PostgreSQL .c source files with libclang.
#
# For each input .c file we walk the AST and collect every external function,
# struct/typedef, and macro the file references.  "External" means the
# symbol's definition lives outside the input set; header files and other .c
# files both qualify.  Each external symbol is emitted as a markdown entry
# with the leading multi-line comment block plus the prototype, struct body,
# or #define text.
#
# Defaults regenerate dev/parse_key_join_external_symbols.md from
# {parse_key_join.c, keyjoincmds.c}.

import argparse
import json
import pathlib
import re
import shlex
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass

import clang.cindex
from clang.cindex import CursorKind, TranslationUnit, TypeKind


TOOL_DIR = pathlib.Path(__file__).resolve().parent
ROOT = TOOL_DIR.parent

DEFAULT_INPUTS = [
    "src/backend/parser/parse_key_join.c",
    "src/backend/commands/keyjoincmds.c",
]
DEFAULT_OUTPUT = pathlib.Path("dev/parse_key_join_external_symbols.md")
DEFAULT_COMPILE_COMMANDS = pathlib.Path(
    "~/build-postgresql-release/compile_commands.json"
).expanduser()
# Prefer Homebrew llvm's libclang; the Apple CommandLineTools one is too old
# for the Python clang.cindex bindings (missing clang_getOffsetOfBase).
DEFAULT_LIBCLANG = pathlib.Path("/opt/homebrew/opt/llvm/lib/libclang.dylib")

# Compile-command flags to drop before handing them to libclang.
_FLAGS_WITH_VALUE = {"-o", "-MT", "-MF", "-MQ", "-MP"}
_DROP_PREFIXES = ("-Werror",)


@dataclass
class Entry:
    name: str
    kind: str            # "function" | "struct" | "macro"
    macro_kind: str = "" # "function-like macro" | "object-like macro"
    rel_path: str = ""
    line: int = 0
    comment: str = ""
    body: str = ""


# ---------------------------------------------------------------------------
# compile_commands.json handling
# ---------------------------------------------------------------------------

_INCLUDE_FLAGS = ("-I", "-isystem", "-iquote", "-idirafter", "-include")


def absolutize_include_paths(flags, cwd):
    """Make any relative -I/-isystem/etc. path absolute against `cwd`."""
    cwd = pathlib.Path(cwd)
    out = []
    i = 0
    n = len(flags)
    while i < n:
        a = flags[i]
        if a in _INCLUDE_FLAGS and i + 1 < n:
            val = flags[i + 1]
            p = pathlib.Path(val)
            if not p.is_absolute():
                val = str((cwd / p).resolve())
            out.append(a)
            out.append(val)
            i += 2
            continue
        matched = False
        for prefix in _INCLUDE_FLAGS:
            if a.startswith(prefix) and len(a) > len(prefix):
                val = a[len(prefix):]
                p = pathlib.Path(val)
                if not p.is_absolute():
                    val = str((cwd / p).resolve())
                out.append(prefix + val)
                matched = True
                break
        if not matched:
            out.append(a)
        i += 1
    return out


def filter_args(args):
    """Return the subset of arguments libclang needs to parse the file."""
    out = []
    skip = False
    for i, a in enumerate(args):
        if i == 0:
            continue          # compiler binary
        if skip:
            skip = False
            continue
        if a in _FLAGS_WITH_VALUE:
            skip = True
            continue
        if any(a.startswith(p) for p in _DROP_PREFIXES):
            continue
        if a in ("-c", "-pipe"):
            continue
        if a.endswith(".c") or a.endswith(".o"):
            continue
        out.append(a)
    return out


def load_compile_commands(path):
    db = {}
    raw = json.loads(pathlib.Path(path).read_text())
    for entry in raw:
        d = pathlib.Path(entry["directory"])
        f = (d / entry["file"]).resolve()
        if "arguments" in entry:
            args = entry["arguments"]
        else:
            args = shlex.split(entry["command"])
        flags = filter_args(args)
        flags = absolutize_include_paths(flags, d)
        db[str(f)] = flags
    return db


def find_resource_dir(libclang_path):
    """Resolve clang's resource directory next to the libclang dylib so libclang
    can find its bundled system headers (stdalign.h, stddef.h, etc.)."""
    candidates = [
        pathlib.Path(libclang_path).parent.parent / "bin" / "clang",
        pathlib.Path("/opt/homebrew/opt/llvm/bin/clang"),
        pathlib.Path("clang"),
    ]
    for c in candidates:
        try:
            r = subprocess.run([str(c), "--print-resource-dir"],
                               capture_output=True, text=True, check=True)
            return r.stdout.strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
    return None


# ---------------------------------------------------------------------------
# AST walking and reference collection
# ---------------------------------------------------------------------------

def walk_input_cursors(tu, input_path_str):
    for cur in tu.cursor.walk_preorder():
        f = cur.location.file
        if f is not None and str(f.name) == input_path_str:
            yield cur


def collect_refs(cursors, refs):
    for cur in cursors:
        if cur.kind == CursorKind.CALL_EXPR:
            ref = cur.referenced
            if ref and ref.kind == CursorKind.FUNCTION_DECL:
                refs["function"].setdefault(ref.spelling, ref)
        elif cur.kind in (CursorKind.TYPE_REF, CursorKind.DECL_REF_EXPR):
            ref = cur.referenced
            if ref and ref.kind in (
                CursorKind.STRUCT_DECL, CursorKind.UNION_DECL,
                CursorKind.TYPEDEF_DECL, CursorKind.ENUM_DECL,
            ):
                refs["struct"].setdefault(ref.spelling, ref)
        elif cur.kind == CursorKind.MACRO_INSTANTIATION:
            ref = cur.referenced
            if ref and ref.kind == CursorKind.MACRO_DEFINITION:
                refs["macro"].setdefault(ref.spelling, ref)


def is_under_repo(path):
    try:
        path.relative_to(ROOT)
        return True
    except ValueError:
        return False


def is_external_ref(defn, input_set):
    f = defn.location.file
    if f is None:
        return False
    p = pathlib.Path(f.name).resolve()
    if str(p) in input_set:
        return False
    if not is_under_repo(p):
        return False
    return True


# ---------------------------------------------------------------------------
# Source text extraction
# ---------------------------------------------------------------------------

def read_file(path, cache):
    key = str(path)
    if key not in cache:
        p = pathlib.Path(path)
        if not p.exists():
            cache[key] = None     # generated header etc. — virtual path only
        else:
            cache[key] = p.read_text()
    return cache[key]


def extract_range(text, sl, sc, el, ec):
    """Extract text from (sl,sc) up to (but not including) (el,ec); 1-indexed."""
    lines = text.splitlines(keepends=True)
    if sl == el:
        return lines[sl - 1][sc - 1:ec - 1]
    parts = [lines[sl - 1][sc - 1:]]
    for i in range(sl, el - 1):
        parts.append(lines[i])
    parts.append(lines[el - 1][:ec - 1])
    return "".join(parts)


def extract_extent(extent, text):
    return extract_range(text, extent.start.line, extent.start.column,
                         extent.end.line, extent.end.column)


def extract_full_lines(extent, text):
    """Like extract_extent but always whole source lines.  Use for struct/typedef
    definitions; libclang sometimes places extent.start in the middle of the
    previous declaration's line, so column-precise extraction yields junk."""
    lines = text.splitlines(keepends=True)
    return "".join(lines[extent.start.line - 1:extent.end.line]).rstrip()


def scan_leading_comment(file_path, def_line, file_cache):
    """Walk backwards from the line above `def_line` to capture an immediately
    preceding /* ... */ block comment.  Returns '' if no such comment exists.
    Used as a fallback when libclang's raw_comment is None (common for typedefs
    wrapping structs, macros, and some declarations).  A single blank line
    between the comment and the declaration is tolerated."""
    text = read_file(file_path, file_cache)
    if text is None:
        return ""
    lines = text.splitlines()
    i = def_line - 2          # 0-indexed line just above def_line (1-indexed)
    blanks = 0
    while i >= 0 and lines[i].strip() == "":
        blanks += 1
        if blanks > 1:
            return ""
        i -= 1
    if i < 0 or not lines[i].rstrip().endswith("*/"):
        return ""
    end = i
    while i >= 0 and "/*" not in lines[i]:
        i -= 1
    if i < 0:
        return ""
    return "\n".join(lines[i:end + 1])


# ---------------------------------------------------------------------------
# .c-file definition lookup via git grep (one batched call)
# ---------------------------------------------------------------------------

def find_c_definitions(funcnames):
    """One git-grep call to locate `^funcname(` for every funcname.
    Returns {name: (rel_path, line)} preferring .c-file matches."""
    if not funcnames:
        return {}
    pattern = "|".join(re.escape(n) for n in funcnames)
    grep_regex = rf"^({pattern})[[:space:]]*\("       # POSIX ERE for git grep
    proc = subprocess.run(
        ["git", "grep", "-nE", grep_regex, "--", "src/"],
        capture_output=True, text=True, cwd=ROOT,
    )
    matches = defaultdict(list)
    line_pat = re.compile(rf"^({pattern})\s*\(")      # Python re uses \s
    for raw in proc.stdout.splitlines():
        try:
            path, lineno, content = raw.split(":", 2)
        except ValueError:
            continue
        m = line_pat.match(content)
        if not m:
            continue
        matches[m.group(1)].append((path, int(lineno)))
    out = {}
    for name, ms in matches.items():
        c_ms = [m for m in ms if m[0].endswith(".c")]
        out[name] = c_ms[0] if c_ms else ms[0]
    return out


# ---------------------------------------------------------------------------
# Entry construction
# ---------------------------------------------------------------------------

def extract_c_function(rel_path, name_line):
    """Extract leading comment + signature (terminated with `;`) from the
    PG-style function definition whose name sits at col 0 of `name_line`."""
    text = (ROOT / rel_path).read_text()
    lines = text.splitlines()
    name_idx = name_line - 1

    decl_start = name_idx
    while decl_start > 0:
        prev = lines[decl_start - 1].rstrip()
        if prev == "" or prev.endswith(("}", "*/", ";")):
            break
        decl_start -= 1

    comment_start = decl_start
    if comment_start > 0 and lines[comment_start - 1].rstrip().endswith("*/"):
        c = comment_start - 1
        while c >= 0:
            if lines[c].lstrip().startswith("/*"):
                comment_start = c
                break
            c -= 1

    body_idx = name_idx
    while body_idx < len(lines) and "{" not in lines[body_idx]:
        body_idx += 1

    comment_lines = lines[comment_start:decl_start] if comment_start < decl_start else []
    sig_lines = lines[decl_start:body_idx]
    comment = "\n".join(comment_lines).rstrip()
    sig = "\n".join(sig_lines).rstrip()
    if not sig.endswith(";"):
        sig += ";"
    return comment, sig, decl_start + 1


def looks_like_struct_typedef(defn, body_text):
    if defn.kind in (CursorKind.STRUCT_DECL, CursorKind.UNION_DECL, CursorKind.ENUM_DECL):
        return True
    if defn.kind == CursorKind.TYPEDEF_DECL:
        u = defn.underlying_typedef_type.get_canonical()
        if u.kind in (TypeKind.RECORD, TypeKind.ENUM):
            return True
        if re.search(r'\b(struct|union|enum)\b', body_text):
            return True
    return False


def detect_macro_kind(defn, text):
    line = text.splitlines()[defn.extent.start.line - 1]
    m = re.search(rf"#\s*define\s+{re.escape(defn.spelling)}(\()?", line)
    return "function-like macro" if (m and m.group(1)) else "object-like macro"


def build_function_entry(defn, c_def, file_cache, input_rel_set):
    name = defn.spelling
    if c_def:
        rel_path, name_line = c_def
        if rel_path in input_rel_set:
            return None  # definition is inside the input set after all
        try:
            comment, body, decl_line = extract_c_function(rel_path, name_line)
            return Entry(name=name, kind="function",
                         rel_path=rel_path, line=decl_line,
                         comment=comment, body=body)
        except Exception as e:
            print(f"  warn: failed to extract {name} from {rel_path}:{name_line}: {e}",
                  file=sys.stderr)
    # Fallback: use libclang's view (typically a static inline in a header)
    f = defn.location.file
    if f is None:
        return None
    path = pathlib.Path(f.name).resolve()
    try:
        rel_path = str(path.relative_to(ROOT))
    except ValueError:
        return None
    text = read_file(path, file_cache)
    if text is None:
        return None
    comment = (defn.raw_comment or "").rstrip()
    body_cur = None
    for child in defn.get_children():
        if child.kind == CursorKind.COMPOUND_STMT:
            body_cur = child
            break
    if body_cur is not None:
        sig = extract_range(text,
                            defn.extent.start.line, defn.extent.start.column,
                            body_cur.extent.start.line, body_cur.extent.start.column)
        sig = sig.rstrip() + ";"
    else:
        sig = extract_extent(defn.extent, text).rstrip()
        if not sig.endswith(";"):
            sig += ";"
    return Entry(name=name, kind="function",
                 rel_path=rel_path, line=defn.location.line,
                 comment=comment, body=sig)


def build_struct_entry(defn, file_cache):
    f = defn.location.file
    if f is None:
        return None
    path = pathlib.Path(f.name).resolve()
    try:
        rel_path = str(path.relative_to(ROOT))
    except ValueError:
        return None
    text = read_file(path, file_cache)
    if text is None:
        return None
    def_line = defn.extent.start.line     # typedef keyword, not closing brace
    body = extract_full_lines(defn.extent, text)
    if not looks_like_struct_typedef(defn, body):
        return None
    if not body.rstrip().endswith(";"):
        body = body.rstrip() + ";"        # libclang typedef extent stops before ';'
    comment = (defn.raw_comment or "").rstrip()
    if not comment:
        comment = scan_leading_comment(path, def_line, file_cache).rstrip()
    return Entry(name=defn.spelling, kind="struct",
                 rel_path=rel_path, line=def_line,
                 comment=comment, body=body)


def build_macro_entry(defn, file_cache):
    f = defn.location.file
    if f is None:
        return None
    path = pathlib.Path(f.name).resolve()
    try:
        rel_path = str(path.relative_to(ROOT))
    except ValueError:
        return None
    text = read_file(path, file_cache)
    if text is None:
        return None
    sl, el = defn.extent.start.line, defn.extent.end.line
    body = "".join(text.splitlines(keepends=True)[sl - 1:el]).rstrip()
    if not body.lstrip().startswith("#"):
        return None
    macro_kind = detect_macro_kind(defn, text)
    def_line = defn.extent.start.line
    comment = (defn.raw_comment or "").rstrip()
    if not comment:
        comment = scan_leading_comment(path, def_line, file_cache).rstrip()
    return Entry(name=defn.spelling, kind="macro", macro_kind=macro_kind,
                 rel_path=rel_path, line=def_line,
                 comment=comment, body=body)


# ---------------------------------------------------------------------------
# Markdown emission
# ---------------------------------------------------------------------------

def write_markdown(path, input_rel_paths, entries):
    files_str = ", ".join(input_rel_paths)
    sources_str = ", ".join(f"`{p}`" for p in input_rel_paths)
    out = []
    out.append(f"# External PostgreSQL Symbols Used by {files_str}\n")
    out.append("\n")
    out.append(f"Source inspected: {sources_str}\n")
    out.append("\n")
    out.append(
        "Scope: source-visible PostgreSQL C function identifiers, "
        "struct/struct-typedef type identifiers, and preprocessor macros "
        "referenced from the source files above, excluding anything defined "
        "inside that set. Static inline functions in headers are included.\n"
    )
    out.append("\n")
    out.append(
        f"Counts: {len(entries['function'])} functions, "
        f"{len(entries['struct'])} struct-like types, "
        f"{len(entries['macro'])} macros.\n"
    )
    out.append("\n")

    for kind, header in (("function", "## Functions"),
                         ("struct", "## Structs"),
                         ("macro", "## Macros")):
        out.append(header + "\n")
        out.append("\n")
        for e in entries[kind]:
            out.append(f"### `{e.name}`\n")
            out.append("\n")
            if kind == "struct":
                out.append("C kind: struct typedef\n")
                out.append("\n")
            elif kind == "macro":
                out.append(f"C kind: {e.macro_kind}\n")
                out.append("\n")
            out.append(f"Definition path: `{e.rel_path}:{e.line}`\n")
            out.append("\n")
            out.append("```c\n")
            if e.comment:
                out.append(e.comment + "\n")
            out.append(e.body + "\n")
            out.append("```\n")
            out.append("\n")

    pathlib.Path(path).write_text("".join(out).rstrip() + "\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", default=None,
                        help="C source files (repo-relative or absolute)")
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--compile-commands", type=pathlib.Path,
                        default=DEFAULT_COMPILE_COMMANDS)
    parser.add_argument("--libclang", type=pathlib.Path, default=DEFAULT_LIBCLANG)
    args = parser.parse_args()

    inputs = args.inputs or DEFAULT_INPUTS

    if args.libclang and args.libclang.exists():
        clang.cindex.Config.set_library_file(str(args.libclang))

    inputs_abs = []
    input_rel = []
    for p in inputs:
        ap = pathlib.Path(p)
        if not ap.is_absolute():
            ap = (ROOT / ap).resolve()
        else:
            ap = ap.resolve()
        if not ap.exists():
            print(f"error: input not found: {ap}", file=sys.stderr)
            return 1
        inputs_abs.append(ap)
        try:
            input_rel.append(str(ap.relative_to(ROOT)))
        except ValueError:
            input_rel.append(str(ap))
    input_abs_set = {str(p) for p in inputs_abs}
    input_rel_set = set(input_rel)

    cdb = load_compile_commands(args.compile_commands)

    resource_dir = find_resource_dir(args.libclang)
    if resource_dir:
        print(f"clang resource-dir: {resource_dir}", file=sys.stderr)

    index = clang.cindex.Index.create()
    refs = {"function": {}, "struct": {}, "macro": {}}

    for input_path in inputs_abs:
        flags = cdb.get(str(input_path))
        if flags is None:
            print(f"error: no compile_commands entry for {input_path}", file=sys.stderr)
            return 1
        if resource_dir:
            flags = [f"-resource-dir={resource_dir}"] + flags
        print(f"parsing {input_path.relative_to(ROOT)}", file=sys.stderr)
        tu = index.parse(
            str(input_path), args=flags,
            options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
        for d in tu.diagnostics:
            if d.severity >= clang.cindex.Diagnostic.Error:
                print(f"  diag: {d.spelling} at {d.location}", file=sys.stderr)
        cursors = list(walk_input_cursors(tu, str(input_path)))
        print(f"  {len(cursors)} cursors in input", file=sys.stderr)
        collect_refs(cursors, refs)

    print(f"raw refs: {len(refs['function'])} func, "
          f"{len(refs['struct'])} type, {len(refs['macro'])} macro",
          file=sys.stderr)

    # Debug: sample some function references and where their definitions live
    sample_names = list(refs["function"].keys())[:8]
    for name in sample_names:
        d = refs["function"][name]
        f = d.location.file
        print(f"  sample func {name}: file={f.name if f else None} line={d.location.line}",
              file=sys.stderr)

    # Filter to externals
    ext = {"function": {}, "struct": {}, "macro": {}}
    for kind in ext:
        for name, defn in refs[kind].items():
            if is_external_ref(defn, input_abs_set):
                ext[kind][name] = defn

    # Batch git-grep for function .c definitions
    c_defs = find_c_definitions(list(ext["function"].keys()))
    print(f"git-grep located {len(c_defs)} .c definitions for "
          f"{len(ext['function'])} external functions", file=sys.stderr)

    file_cache = {}
    entries = {"function": [], "struct": [], "macro": []}
    for name, defn in ext["function"].items():
        e = build_function_entry(defn, c_defs.get(name), file_cache, input_rel_set)
        if e:
            entries["function"].append(e)
    for name, defn in ext["struct"].items():
        e = build_struct_entry(defn, file_cache)
        if e:
            entries["struct"].append(e)
    for name, defn in ext["macro"].items():
        e = build_macro_entry(defn, file_cache)
        if e:
            entries["macro"].append(e)

    for k in entries:
        seen = set()
        unique = []
        for e in sorted(entries[k], key=lambda e: (e.name.lower(), e.name)):
            if e.name in seen:
                continue
            seen.add(e.name)
            unique.append(e)
        entries[k] = unique

    print(f"emitted: {len(entries['function'])} func, "
          f"{len(entries['struct'])} struct, {len(entries['macro'])} macro",
          file=sys.stderr)

    output_path = args.output
    if not output_path.is_absolute():
        output_path = ROOT / output_path
    write_markdown(output_path, input_rel, entries)
    print(f"=> wrote {output_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
