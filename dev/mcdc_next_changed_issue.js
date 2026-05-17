#!/usr/bin/env node
/*
 * Print the first visible llvm-cov HTML issue on a changed line outside the
 * full-file MC/DC targets, or every matching issue with --all.
 *
 * mcdc_next_issue.js follows the generated report's queue for the full-file
 * targets.  This helper uses the same DOM/control.js model, but builds a
 * combined queue from the other generated source reports and filters it to
 * lines changed by the current branch relative to BASE_REF, defaulting to
 * master.
 */

const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const SRC = path.resolve(process.env.SRC || path.join(__dirname, ".."));
const BASE_REF = process.env.BASE_REF || "master";
const REPORT_DIR =
	process.env.REPORT_DIR || "/Users/joel/build-postgresql-mcdc/mcdc-report";
const HTML_DIR = process.env.HTML_DIR || path.join(REPORT_DIR, "html");
const DEFAULT_EXCLUDED_SOURCES = [
	"src/backend/parser/parse_key_join.c",
	"src/backend/commands/keyjoincmds.c",
];
const EXCLUDED_SOURCES = new Set(
	(process.env.EXCLUDE_SOURCES ?
		process.env.EXCLUDE_SOURCES.split(/[,\s]+/) :
		DEFAULT_EXCLUDED_SOURCES)
		.filter(Boolean)
		.map(normalizePath)
);

function die(message) {
	process.stderr.write(`error: ${message}\n`);
	process.exit(1);
}

function normalizePath(filePath) {
	return filePath.split(path.sep).join("/");
}

function unescapeHtml(text) {
	return text
		.replace(/&quot;/g, "\"")
		.replace(/&apos;/g, "'")
		.replace(/&#39;/g, "'")
		.replace(/&lt;/g, "<")
		.replace(/&gt;/g, ">")
		.replace(/&amp;/g, "&");
}

function stripTags(html) {
	return unescapeHtml(html.replace(/<[^>]*>/g, ""));
}

function classSet(text) {
	return new Set(text.trim().split(/\s+/).filter(Boolean));
}

function hasClasses(classes, required) {
	return required.every((name) => classes.has(name));
}

function parseCoreSelector(selector) {
	const tagMatch = /^[a-z]+/.exec(selector);
	const tag = tagMatch ? tagMatch[0] : null;
	const classes = [];
	const classRe = /\.([A-Za-z0-9_-]+)/g;

	for (const match of selector.matchAll(classRe))
		classes.push(match[1]);

	return {classes, tag};
}

function parseRows(reportHtml, sourcePath) {
	const rows = [];
	const rowRe =
		/<tr><td class='line-number'><a name='L(\d+)'[\s\S]*?<\/td><td class='([^']+)'>([\s\S]*?)<\/td><td class='code'><pre>([\s\S]*?)<\/pre>([\s\S]*?)<\/td><\/tr>/g;

	for (const match of reportHtml.matchAll(rowRe)) {
		rows.push({
			code: stripTags(match[4]),
			countClass: match[2],
			countText: stripTags(match[3]).trim(),
			line: Number(match[1]),
			position: match.index,
			rowHtml: match[0],
			sourcePath,
		});
	}

	return rows;
}

function parseBranchIssue(branchHtml, row, position) {
	const branchRe = /Branch\s*\((.*?)\):\s*\[(.*?)\]/gs;
	const records = [];

	for (const branchMatch of branchHtml.matchAll(branchRe)) {
		const location = stripTags(branchMatch[1]).trim();
		const locMatch = /^(\d+):(\d+)$/.exec(location);
		const branchBody = branchMatch[2];
		const branchText = stripTags(branchBody).replace(/\s+/g, " ");
		const trueMatch = /True:\s*([^,\]]+)/.exec(branchText);
		const falseMatch = /False:\s*([^,\]]+)/.exec(branchText);

		if (!locMatch || !trueMatch || !falseMatch)
			continue;

		const redRe =
			/<span class='([^']*)'>(True|False)<\/span>:\s*<span class='uncovered-line'>0<\/span>/g;

		for (const redMatch of branchBody.matchAll(redRe)) {
			if (!hasClasses(classSet(redMatch[1]), ["red", "branch"]))
				continue;

			records.push({
				code: row.code,
				column: Number(locMatch[2]),
				direction: redMatch[2],
				falseCount: falseMatch[1].trim(),
				line: Number(locMatch[1]),
				position: position + branchMatch.index + redMatch.index,
				sourcePath: row.sourcePath,
				trueCount: trueMatch[1].trim(),
				type: "branch",
			});
		}
	}

	return records;
}

function parseIssues(reportHtml, sourcePath) {
	const issues = [];

	for (const row of parseRows(reportHtml, sourcePath)) {
		if (row.countClass === "uncovered-line") {
			issues.push({
				code: row.code,
				count: row.countText,
				line: row.line,
				position: row.position,
				sourcePath,
				type: "line",
			});
		}

		const spanRe = /<span class='([^']*)'>([\s\S]*?)<\/span>/g;

		for (const spanMatch of row.rowHtml.matchAll(spanRe)) {
			const classes = classSet(spanMatch[1]);

			if (hasClasses(classes, ["red", "region"])) {
				issues.push({
					code: row.code,
					line: row.line,
					position: row.position + spanMatch.index,
					region: stripTags(spanMatch[2]).trim(),
					sourcePath,
					type: "region",
				});
			}
		}

		issues.push(...parseBranchIssue(row.rowHtml, row, row.position));
	}

	return issues.sort((a, b) => a.position - b.position);
}

class ClassList {
	constructor(element) {
		this.element = element;
	}

	add(...names) {
		for (const name of names)
			this.element.classes.add(name);
	}

	remove(...names) {
		for (const name of names)
			this.element.classes.delete(name);
	}
}

class IssueElement {
	constructor(issue, index) {
		this.classes = new Set();
		this.classList = new ClassList(this);
		this.index = index;
		this.issue = issue;
		this.tag = issue.type === "line" ? "td" : "span";

		if (issue.type === "line")
			this.classes.add("uncovered-line");
		else if (issue.type === "region")
			this.classes = new Set(["red", "region"]);
		else if (issue.type === "branch")
			this.classes = new Set(["red", "branch"]);
	}

	matches(selector) {
		let expectedSeen = null;
		let core = selector;

		if (core.endsWith(":not(.seen)")) {
			expectedSeen = false;
			core = core.slice(0, -":not(.seen)".length);
		} else if (core.endsWith(".seen")) {
			expectedSeen = true;
			core = core.slice(0, -".seen".length);
		}

		const parsed = parseCoreSelector(core);

		if (parsed.tag !== null && parsed.tag !== this.tag)
			return false;
		if (!hasClasses(this.classes, parsed.classes))
			return false;
		if (expectedSeen !== null &&
			this.classes.has("seen") !== expectedSeen)
			return false;

		return true;
	}

	scrollIntoView() {
	}
}

function buildDocument(elements) {
	const scrollTarget = {scrollIntoView() {}};

	function selected() {
		return elements.find((element) => element.classes.has("selected")) || null;
	}

	return {
		addEventListener() {
		},
		querySelector(selector) {
			if (selector === ".selected")
				return selected();
			if (selector === "tr:has(.selected) td.line-number")
				return selected() ? scrollTarget : null;

			return elements.find((element) => element.matches(selector)) || null;
		},
		querySelectorAll(selector) {
			return elements.filter((element) => element.matches(selector));
		},
	};
}

function findControlJs(htmlDir) {
	let dir = htmlDir;

	for (;;) {
		const candidate = path.join(dir, "control.js");

		if (fs.existsSync(candidate))
			return candidate;

		const parent = path.dirname(dir);

		if (parent === dir)
			break;
		dir = parent;
	}

	die(`could not find control.js above ${htmlDir}`);
}

function selectedElement(elements) {
	return elements.find((element) => element.classes.has("selected")) || null;
}

function parseSourcePath(reportHtml, htmlPath) {
	const titleMatch =
		/<div class='source-name-title'><pre>(.*?)<\/pre><\/div>/.exec(reportHtml);

	return titleMatch ? stripTags(titleMatch[1]) : htmlPath;
}

function sourceRelPath(sourcePath) {
	const relPath = path.relative(SRC, path.resolve(sourcePath));

	if (relPath !== "" && !relPath.startsWith("..") && !path.isAbsolute(relPath))
		return normalizePath(relPath);

	return normalizePath(sourcePath);
}

function walkHtmlFiles(dir, result = []) {
	for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
		const fullPath = path.join(dir, entry.name);

		if (entry.isDirectory())
			walkHtmlFiles(fullPath, result);
		else if (entry.isFile() && entry.name.endsWith(".html") &&
				 entry.name !== "index.html")
			result.push(fullPath);
	}

	return result;
}

function reportPathsFromIndex(htmlDir) {
	const indexPath = path.join(htmlDir, "index.html");

	if (!fs.existsSync(indexPath))
		return [];

	const indexHtml = fs.readFileSync(indexPath, "utf8");
	const linkRe = /<a href='([^']+\.html)'>([\s\S]*?)<\/a>/g;
	const reports = [];

	for (const match of indexHtml.matchAll(linkRe)) {
		const href = unescapeHtml(match[1]);

		if (!href.startsWith("coverage/"))
			continue;
		reports.push(path.resolve(htmlDir, href));
	}

	return reports;
}

function reportPaths() {
	if (!fs.existsSync(HTML_DIR))
		die(`HTML report directory not found: ${HTML_DIR}`);

	const reports = reportPathsFromIndex(HTML_DIR);

	if (reports.length > 0)
		return reports;

	return walkHtmlFiles(HTML_DIR).sort();
}

function parseGitDiffChangedLines() {
	const diff = childProcess.spawnSync(
		"git",
		[
			"-C",
			SRC,
			"diff",
			"--unified=0",
			"--no-color",
			"--no-ext-diff",
			`${BASE_REF}...HEAD`,
			"--",
		],
		{encoding: "utf8", maxBuffer: 128 * 1024 * 1024}
	);

	if (diff.error)
		die(diff.error.message);
	if (diff.status !== 0)
		die(diff.stderr.trim() || `git diff failed with status ${diff.status}`);

	const changedLines = new Map();
	let currentFile = null;
	let newLine = null;

	function addLine(filePath, line) {
		if (!changedLines.has(filePath))
			changedLines.set(filePath, new Set());
		changedLines.get(filePath).add(line);
	}

	for (const line of diff.stdout.split(/\n/)) {
		if (line.startsWith("+++ ")) {
			const filePath = line.slice(4).trim();

			if (filePath === "/dev/null")
				currentFile = null;
			else if (filePath.startsWith("b/"))
				currentFile = normalizePath(filePath.slice(2));
			else
				currentFile = normalizePath(filePath);
			newLine = null;
			continue;
		}

		const hunkMatch = /^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@/.exec(line);

		if (hunkMatch) {
			newLine = Number(hunkMatch[1]);
			continue;
		}

		if (currentFile === null || newLine === null)
			continue;

		if (line.startsWith("+")) {
			addLine(currentFile, newLine);
			newLine++;
		} else if (line.startsWith("-")) {
			continue;
		} else if (line.startsWith(" ")) {
			newLine++;
		}
	}

	return changedLines;
}

function collectChangedIssues(changedLines) {
	const issues = [];
	let fileOrder = 0;

	for (const htmlPath of reportPaths()) {
		if (!fs.existsSync(htmlPath))
			continue;

		const reportHtml = fs.readFileSync(htmlPath, "utf8");
		const sourcePath = parseSourcePath(reportHtml, htmlPath);
		const relPath = sourceRelPath(sourcePath);

		if (EXCLUDED_SOURCES.has(relPath))
			continue;

		const changed = changedLines.get(relPath);

		if (!changed)
			continue;

		for (const issue of parseIssues(reportHtml, sourcePath)) {
			if (!changed.has(issue.line))
				continue;

			issues.push({
				...issue,
				fileOrder,
				relPath,
			});
		}

		fileOrder++;
	}

	return issues.sort((a, b) => {
		if (a.fileOrder !== b.fileOrder)
			return a.fileOrder - b.fileOrder;
		return a.position - b.position;
	});
}

function loadReport() {
	const changedLines = parseGitDiffChangedLines();
	const issues = collectChangedIssues(changedLines);
	const elements = issues.map((issue, index) => new IssueElement(issue, index));
	const controlJs = findControlJs(HTML_DIR);
	const context = {
		document: buildDocument(elements),
	};

	vm.runInNewContext(fs.readFileSync(controlJs, "utf8"), context, {
		filename: controlJs,
	});

	for (const name of ["next_line", "next_region", "next_branch"]) {
		if (typeof context[name] !== "function")
			die(`${controlJs} did not define ${name}()`);
	}

	return {context, elements};
}

function invoke(report, helper) {
	report.context[helper]();
	return selectedElement(report.elements);
}

function printIssue(element) {
	if (!element) {
		process.stdout.write(
			"No uncovered line, region, or branch target found on changed lines " +
			`outside ${Array.from(EXCLUDED_SOURCES).join(", ")}.\n`
		);
		return;
	}

	const issue = element.issue;
	const source = `${issue.sourcePath}:${issue.line}` +
		(issue.column ? `:${issue.column}` : "");

	process.stdout.write(`Next uncovered changed-line ${issue.type} (${element.index + 1})
  source: ${source}
  relative: ${issue.relPath}
`);

	if (issue.type === "line") {
		process.stdout.write(`  count: ${issue.count}
`);
	} else if (issue.type === "region") {
		process.stdout.write(`  region: ${issue.region}
`);
	} else {
		process.stdout.write(`  direction: ${issue.direction}
  counts: True=${issue.trueCount} False=${issue.falseCount}
`);
	}

	process.stdout.write(`  code: ${issue.code.trim()}
`);
}

function printAllIssues(elements) {
	if (elements.length === 0) {
		process.stdout.write(
			"No uncovered line, region, or branch target found on changed lines " +
			`outside ${Array.from(EXCLUDED_SOURCES).join(", ")}.\n`
		);
		return;
	}

	const counts = {branch: 0, line: 0, region: 0};

	for (const element of elements)
		counts[element.issue.type]++;

	process.stdout.write(
		`Uncovered changed-line issues: ${elements.length}` +
		` (lines=${counts.line}, regions=${counts.region}, ` +
		`branches=${counts.branch})\n`
	);

	for (const element of elements) {
		const issue = element.issue;
		const source = `${issue.sourcePath}:${issue.line}` +
			(issue.column ? `:${issue.column}` : "");

		process.stdout.write(`\nUncovered changed-line ${issue.type} (${element.index + 1})
  source: ${source}
  relative: ${issue.relPath}
`);

		if (issue.type === "line") {
			process.stdout.write(`  count: ${issue.count}
`);
		} else if (issue.type === "region") {
			process.stdout.write(`  region: ${issue.region}
`);
		} else {
			process.stdout.write(`  direction: ${issue.direction}
  counts: True=${issue.trueCount} False=${issue.falseCount}
`);
		}

		process.stdout.write(`  code: ${issue.code.trim()}
`);
	}
}

function parseArgs() {
	if (process.argv.length === 2)
		return {all: false};
	if (process.argv.length === 3 && process.argv[2] === "--all")
		return {all: true};

	die(`${path.basename(process.argv[1])} accepts only optional --all`);
}

function main() {
	const options = parseArgs();

	const report = loadReport();

	if (options.all) {
		printAllIssues(report.elements);
		return;
	}

	const selected = [
		invoke(report, "next_line"),
		invoke(report, "next_region"),
		invoke(report, "next_branch"),
	].filter(Boolean).sort((a, b) => a.index - b.index)[0] || null;

	printIssue(selected);
}

main();
