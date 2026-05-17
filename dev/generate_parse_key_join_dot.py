#!/usr/bin/env python3
#
# Generate a DOT call graph for src/backend/parser/parse_key_join.c.
#
# The graph intentionally hides "helper" leaf functions:
#
# A helper function is a local function that has at least one incoming edge
# from another visible local function, and zero outgoing edges to local
# functions.  In other words, it is called by this file but calls no other
# local function itself.
#
# A self-recursive function is not a helper under this definition, because it
# has an outgoing local edge to itself.  A function with no callers is also not
# hidden, because it is an entry point or dead/unreferenced local surface rather
# than a leaf helper.
#
# Edges come from the C source and cscope only.  Existing "Called by:" comments
# and existing graph artifacts are not read.

import argparse
import bisect
import colorsys
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict


TOOL_DIR = pathlib.Path(__file__).resolve().parent
ROOT = TOOL_DIR.parent
DEFAULT_SOURCE = pathlib.Path("src/backend/parser/parse_key_join.c")
DEFAULT_OUTPUT = TOOL_DIR / "parse_key_join_graph.dot"
SELF_CYCLE_COLOR = "#932fb1"

SHARED_MATCH_OUTPUT_CALLEES = (
    "append_dependencies_unique",
    "filter_conjunct_matches_key_positions",
    "remap_filter_conjunct",
    "select_key_position_parts",
)

SURFACE_MAPPING_CALLEES = (
    "direct_var_from_node",
    "jtnode_surface_rtindex",
    "map_var_to_jtnode_surface",
)


class SourceMap:
    def __init__(self, text):
        self.text = text
        self.stripped = strip_comments(text)
        self.line_starts = [0]
        for match in re.finditer(r"\n", self.stripped):
            self.line_starts.append(match.end())
        self.comment_only_lines = comment_only_lines(text)

    def line_number(self, offset):
        return bisect.bisect_right(self.line_starts, offset)


def strip_comments(text):
    """Remove C comments while preserving line numbers and offsets."""
    out = []
    index = 0
    in_block = False
    in_line = False

    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""

        if in_block:
            if char == "*" and next_char == "/":
                out.append(" ")
                out.append(" ")
                index += 2
                in_block = False
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
            continue

        if in_line:
            if char == "\n":
                out.append("\n")
                in_line = False
            else:
                out.append(" ")
            index += 1
            continue

        if char == "/" and next_char == "*":
            out.append(" ")
            out.append(" ")
            index += 2
            in_block = True
        elif char == "/" and next_char == "/":
            out.append(" ")
            out.append(" ")
            index += 2
            in_line = True
        else:
            out.append(char)
            index += 1

    return "".join(out)


def comment_only_lines(text):
    """Return lines that become empty after removing comments."""
    stripped = strip_comments(text)
    result = set()
    for lineno, (raw, clean) in enumerate(zip(text.splitlines(),
                                              stripped.splitlines()), start=1):
        if raw.strip() and not clean.strip():
            result.add(lineno)
    return result


def find_matching_paren(text, open_offset):
    depth = 0
    index = open_offset
    quote = None
    escaped = False

    while index < len(text):
        char = text[index]
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        else:
            if char in ("'", '"'):
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    return index
        index += 1
    return None


def discover_function_definitions(source_map):
    """Find top-level PostgreSQL-style C function definitions."""
    functions = {}
    text = source_map.stripped

    for match in re.finditer(r"(?m)^([A-Za-z_][A-Za-z0-9_]*)\s*\(", text):
        name = match.group(1)
        close = find_matching_paren(text, match.end() - 1)
        if close is None:
            continue

        index = close + 1
        while index < len(text) and text[index].isspace():
            index += 1

        if index < len(text) and text[index] == "{":
            if name in functions:
                raise SystemExit(f"duplicate function definition: {name}")
            functions[name] = source_map.line_number(match.start())

    if not functions:
        raise SystemExit("no function definitions found")
    return functions


def run_cscope(source_path, functions):
    cscope = shutil.which("cscope")
    if cscope is None:
        raise SystemExit("cscope not found in PATH")

    with tempfile.TemporaryDirectory(prefix="parse-key-join-cscope-") as tmp:
        tmp_path = pathlib.Path(tmp)
        (tmp_path / "cscope.files").write_text(str(source_path) + "\n")
        subprocess.run([cscope, "-b", "-q", "-k"],
                       cwd=tmp_path, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True)

        edges = set()
        edge_lines = {}
        for callee in sorted(functions):
            for caller, lineno in cscope_query(tmp_path, cscope, "-3", callee):
                if caller in functions:
                    add_edge(edges, edge_lines, caller, callee, lineno)

            for caller, lineno in cscope_query(tmp_path, cscope, "-0", callee):
                if caller not in functions:
                    continue
                if lineno == functions[callee]:
                    continue
                add_edge(edges, edge_lines, caller, callee, lineno)

    return edges, edge_lines


def cscope_query(cwd, cscope, mode, symbol):
    proc = subprocess.run([cscope, "-d", "-L", mode, symbol],
                          cwd=cwd, check=False,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True)
    if proc.returncode not in (0, 1):
        raise SystemExit(proc.stderr.strip() or
                         f"cscope {mode} {symbol} failed")

    for line in proc.stdout.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) < 4:
            continue
        _path, caller, lineno_text, _source = parts
        try:
            lineno = int(lineno_text)
        except ValueError:
            continue
        yield caller, lineno


def add_edge(edges, edge_lines, caller, callee, lineno):
    edge = (caller, callee)
    edges.add(edge)
    edge_lines.setdefault(edge, set()).add(lineno)


def filter_helpers(functions, edges):
    nodes = set(functions)
    incoming = defaultdict(set)
    outgoing = defaultdict(set)
    for caller, callee in edges:
        incoming[callee].add(caller)
        outgoing[caller].add(callee)

    helpers = {
        node for node in nodes
        if incoming[node] and not outgoing[node]
    }
    visible = nodes - helpers
    filtered_edges = {
        edge for edge in edges
        if edge[0] in visible and edge[1] in visible
    }
    return visible, filtered_edges, helpers


def strongly_connected_components(nodes, edges):
    graph = adjacency(nodes, edges)
    index = 0
    stack = []
    on_stack = set()
    indices = {}
    lowlinks = {}
    components = []

    def visit(node):
        nonlocal index
        indices[node] = index
        lowlinks[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)

        for target in graph[node]:
            if target not in indices:
                visit(target)
                lowlinks[node] = min(lowlinks[node], lowlinks[target])
            elif target in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[target])

        if lowlinks[node] == indices[node]:
            component = []
            while True:
                current = stack.pop()
                on_stack.remove(current)
                component.append(current)
                if current == node:
                    break
            components.append(component)

    for node in sorted(nodes):
        if node not in indices:
            visit(node)

    return components


def adjacency(nodes, edges):
    graph = {node: [] for node in nodes}
    for caller, callee in sorted(edges):
        graph[caller].append(callee)
    return graph


def enumerate_cycles(nodes, edges):
    graph = adjacency(nodes, edges)
    cycles = {}

    for component in strongly_connected_components(nodes, edges):
        component_set = set(component)
        internal_edges = {
            edge for edge in edges
            if edge[0] in component_set and edge[1] in component_set
        }
        if len(component) == 1:
            node = component[0]
            if (node, node) in internal_edges:
                cycles[(node,)] = (node,)
            continue

        for start in sorted(component):
            dfs_cycle(start, start, component_set, graph, [], set(), cycles)

    return [list(cycle) for cycle in sorted(cycles)]


def dfs_cycle(start, node, component, graph, path, seen, cycles):
    path.append(node)
    seen.add(node)

    for target in graph[node]:
        if target not in component:
            continue
        if target == start:
            cycles[canonical_cycle(path)] = tuple(path)
        elif target not in seen:
            dfs_cycle(start, target, component, graph, path, seen, cycles)

    seen.remove(node)
    path.pop()


def canonical_cycle(nodes):
    cycle = tuple(nodes)
    rotations = [cycle[index:] + cycle[:index] for index in range(len(cycle))]
    return min(rotations)


def cycle_edges(cycle):
    if len(cycle) == 1:
        return [(cycle[0], cycle[0])]
    return [
        (cycle[index], cycle[(index + 1) % len(cycle)])
        for index in range(len(cycle))
    ]


def assign_cycle_colors(cycles):
    multi = [cycle for cycle in cycles if len(cycle) > 1]
    colors = {}
    for index, cycle in enumerate(multi):
        colors[tuple(cycle)] = hsl_color(index, len(multi))
    for cycle in cycles:
        if len(cycle) == 1:
            colors[tuple(cycle)] = SELF_CYCLE_COLOR
    return colors


def hsl_color(index, count):
    hue = index / max(count, 1)
    red, green, blue = colorsys.hls_to_rgb(hue, 0.46, 0.72)
    return "#{:02x}{:02x}{:02x}".format(
        round(red * 255),
        round(green * 255),
        round(blue * 255),
    )


def dot_quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_analysis_subgraph(out, cluster_name, label, selected_nodes,
                            color, fontcolor, bgcolor):
    out.write(f"  subgraph {cluster_name} {{\n")
    out.write(f"    label={dot_quote(label)};\n")
    out.write("    style=\"rounded,dashed\";\n")
    out.write(f"    color={dot_quote(color)};\n")
    out.write(f"    fontcolor={dot_quote(fontcolor)};\n")
    out.write(f"    bgcolor={dot_quote(bgcolor)};\n")
    out.write("    margin=14;\n")
    for node in selected_nodes:
        out.write(
            f"    {dot_quote(node)} "
            f"[fillcolor={dot_quote(bgcolor)}, color={dot_quote(color)}];\n"
        )
    out.write("  }\n")


def write_hard_coded_subgraphs(out, nodes):
    subgraphs = (
        ("cluster_shared_match_output_callees",
         "match proof and output facts",
         SHARED_MATCH_OUTPUT_CALLEES,
         "#d97706", "#92400e", "#fff7ed"),
        ("cluster_surface_mapping_callees",
         "jointree surface mapping",
         SURFACE_MAPPING_CALLEES,
         "#0f766e", "#115e59", "#f0fdfa"),
    )

    selected_subgraphs = [
        (cluster_name, label, [node for node in candidates if node in nodes],
         color, fontcolor, bgcolor)
        for cluster_name, label, candidates, color, fontcolor, bgcolor in subgraphs
    ]
    selected_subgraphs = [
        subgraph for subgraph in selected_subgraphs
        if subgraph[2]
    ]

    if not selected_subgraphs:
        return

    out.write("\n  // Hard-coded analysis subgraphs.\n")
    for index, subgraph in enumerate(selected_subgraphs):
        if index > 0:
            out.write("\n")
        write_analysis_subgraph(out, *subgraph)


def root_nodes(nodes, edges):
    incoming = {
        callee for caller, callee in edges
        if caller != callee
    }
    return set(nodes) - incoming


def write_dot(path, source_rel, nodes, edges, helpers, cycles, colors):
    cycle_edge_set = {
        edge
        for cycle in cycles
        for edge in cycle_edges(cycle)
    }
    roots = root_nodes(nodes, edges)

    with path.open("w") as out:
        out.write(f"// Generated call graph for {source_rel}\n")
        out.write("// Helper rule: incoming local edges and zero outgoing local edges.\n")
        out.write("// Self-recursive functions are retained because they call themselves.\n")
        out.write(f"// Hidden helpers: {len(helpers)}\n")
        out.write("// Thicker node borders mark root functions with no local parents.\n")
        out.write("// Gray edges are acyclic ordinary caller -> callee edges.\n")
        out.write("// Colored edges are directed cycle edges.\n")
        out.write("digraph parse_key_join {\n")
        out.write("  graph [\n")
        out.write("    rankdir=TB,\n")
        out.write("    newrank=true,\n")
        out.write("    overlap=false,\n")
        out.write("    outputorder=edgesfirst\n")
        out.write("  ];\n")
        out.write("  node [\n")
        out.write("    shape=box,\n")
        out.write("    style=\"rounded,filled\",\n")
        out.write("    fillcolor=\"white\",\n")
        out.write("    color=\"#8792a2\",\n")
        out.write("    fontname=\"Helvetica\",\n")
        out.write("    fontsize=10\n")
        out.write("  ];\n")
        out.write("  edge [\n")
        out.write("    color=\"#9ca3af\",\n")
        out.write("    arrowsize=0.7,\n")
        out.write("    penwidth=1.1\n")
        out.write("  ];\n\n")

        out.write("  // Non-helper local functions.\n")
        for node in sorted(nodes):
            if node in roots:
                out.write(f"  {dot_quote(node)} [color=\"black\", penwidth=2.0];\n")
            else:
                out.write(f"  {dot_quote(node)};\n")

        write_hard_coded_subgraphs(out, nodes)

        out.write("\n  // Acyclic caller -> callee edges.\n")
        for caller, callee in sorted(edges):
            if (caller, callee) in cycle_edge_set:
                continue
            out.write(f"  {dot_quote(caller)} -> {dot_quote(callee)};\n")

        out.write("\n  // Colored directed cycle edges.\n")
        for index, cycle in enumerate(cycles, start=1):
            color = colors[tuple(cycle)]
            for caller, callee in cycle_edges(cycle):
                out.write(
                    f"  {dot_quote(caller)} -> {dot_quote(callee)} "
                    f"[color={dot_quote(color)}, fontcolor={dot_quote(color)}, "
                    "penwidth=2.6, arrowsize=0.85, "
                    f"tooltip={dot_quote(f'cycle-{index}')}];\n"
                )
        out.write("}\n")


def degree_groups(nodes, edges):
    incoming = defaultdict(set)
    outgoing = defaultdict(set)
    self_recursive = set()

    for caller, callee in edges:
        if caller == callee:
            self_recursive.add(caller)
        else:
            incoming[callee].add(caller)
            outgoing[caller].add(callee)

    groups = defaultdict(list)
    for node in sorted(nodes):
        groups[(node in self_recursive,
                len(incoming[node]),
                len(outgoing[node]))].append(node)
    return groups


def print_degree_group_section(nodes, edges, indent):
    groups = degree_groups(nodes, edges)

    for self_recursive in (False, True):
        matching_groups = [
            item for item in sorted(groups.items())
            if item[0][0] == self_recursive
        ]
        if not matching_groups:
            continue
        print(f"{indent}self-recursive: {'yes' if self_recursive else 'no'}")
        for (_self_recursive, parents, children), functions in matching_groups:
            joined = ", ".join(functions)
            print(f"{indent}  parents={parents} children={children}: {joined}")


def print_degree_groups(nodes, edges, helpers, all_edges):
    print("degree groups:")
    print("  visible functions:")
    print_degree_group_section(nodes, edges, "    ")
    print("  hidden helpers:")
    print_degree_group_section(helpers, all_edges, "    ")


def validate(functions, all_edges, edge_lines, nodes, edges, helpers,
             cycles, source_map):
    if len(all_edges) != len(set(all_edges)):
        raise SystemExit("duplicate edges before filtering")
    if len(edges) != len(set(edges)):
        raise SystemExit("duplicate edges after filtering")

    known = set(functions)
    for caller, callee in all_edges:
        if caller not in known or callee not in known:
            raise SystemExit(f"unknown endpoint before filtering: {caller} -> {callee}")
        for lineno in edge_lines.get((caller, callee), ()):
            if lineno in source_map.comment_only_lines:
                raise SystemExit(f"edge came from comment-only line {lineno}: "
                                 f"{caller} -> {callee}")

    for caller, callee in edges:
        if caller not in nodes or callee not in nodes:
            raise SystemExit(f"unknown endpoint after filtering: {caller} -> {callee}")

    incoming = defaultdict(set)
    outgoing = defaultdict(set)
    for caller, callee in all_edges:
        incoming[callee].add(caller)
        outgoing[caller].add(callee)
    expected_helpers = {
        node for node in functions
        if incoming[node] and not outgoing[node]
    }
    if helpers != expected_helpers:
        raise SystemExit("hidden helper set does not match helper rule")

    edge_set = set(edges)
    for cycle in cycles:
        for edge in cycle_edges(cycle):
            if edge not in edge_set:
                raise SystemExit(f"cycle edge is absent from graph: {edge[0]} -> {edge[1]}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate a non-helper DOT call graph for parse_key_join.c.")
    parser.add_argument("--source", default=str(DEFAULT_SOURCE),
                        help="C source file to analyze")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT),
                        help="DOT output path")
    return parser.parse_args()


def resolve_repo_path(raw_path):
    path = pathlib.Path(raw_path)
    if path.is_absolute():
        return path
    return ROOT / path


def main():
    args = parse_args()
    source_path = resolve_repo_path(args.source).resolve()
    output_path = resolve_repo_path(args.output).resolve()
    try:
        source_rel = source_path.relative_to(ROOT)
    except ValueError:
        source_rel = source_path

    if not source_path.exists():
        raise SystemExit(f"source file not found: {source_path}")

    source_map = SourceMap(source_path.read_text())
    functions = discover_function_definitions(source_map)
    all_edges, edge_lines = run_cscope(source_path, functions)
    nodes, edges, helpers = filter_helpers(functions, all_edges)
    cycles = enumerate_cycles(nodes, edges)
    colors = assign_cycle_colors(cycles)

    validate(functions, all_edges, edge_lines, nodes, edges, helpers,
             cycles, source_map)
    write_dot(output_path, source_rel, nodes, edges, helpers, cycles, colors)

    print(f"source: {source_rel}")
    print(f"functions: {len(functions)}")
    print(f"visible functions: {len(nodes)}")
    print(f"hidden helpers: {len(helpers)}")
    print(f"edges: {len(edges)}")
    print(f"cycles: {len(cycles)}")
    print_degree_groups(nodes, edges, helpers, all_edges)
    try:
        output_rel = output_path.relative_to(ROOT)
    except ValueError:
        output_rel = output_path
    print(f"output: {output_rel}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        if exc.stderr:
            sys.stderr.write(exc.stderr)
        raise SystemExit(exc.returncode)
