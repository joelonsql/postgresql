#!/usr/bin/env node
/*
 * Print the first visible issue selected by llvm-cov HTML's navigation
 * helpers for full-file MC/DC targets, or every visible issue with --all.
 *
 * The llvm-cov report already defines the queue semantics in control.js:
 * next_line(), next_region(), and next_branch() each select the first matching
 * uncovered element that has not been marked seen.  This script evaluates that
 * generated JavaScript against a minimal DOM model so command-line coverage
 * triage follows the same ordering as the report's links.
 */

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const SRC = path.resolve(process.env.SRC || path.join(__dirname, ".."));
const REPORT_DIR =
	process.env.REPORT_DIR || "/Users/joel/build-postgresql-mcdc/mcdc-report";
const HTML_DIR = process.env.HTML_DIR || path.join(REPORT_DIR, "html");
const FULL_FILE_SOURCES = [
	"src/backend/parser/parse_key_join.c",
	"src/backend/commands/keyjoincmds.c",
];

function htmlReportPath(sourceRelPath) {
	const sourcePath = path.resolve(SRC, sourceRelPath);
	const reportRelPath = sourcePath.split(path.sep).filter(Boolean);

	return path.join(HTML_DIR, "coverage", ...reportRelPath) + ".html";
}

const HTML_REPORTS = FULL_FILE_SOURCES.map(htmlReportPath);

function die(message) {
	process.stderr.write(`error: ${message}\n`);
	process.exit(1);
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

function findControlJs(htmlPath) {
	let dir = path.dirname(htmlPath);

	for (;;) {
		const candidate = path.join(dir, "control.js");

		if (fs.existsSync(candidate))
			return candidate;

		const parent = path.dirname(dir);

		if (parent === dir)
			break;
		dir = parent;
	}

	die(`could not find control.js above ${htmlPath}`);
}

function selectedElement(elements) {
	return elements.find((element) => element.classes.has("selected")) || null;
}

function parseSourcePath(reportHtml, htmlPath) {
	const titleMatch =
		/<div class='source-name-title'><pre>(.*?)<\/pre><\/div>/.exec(reportHtml);

	return titleMatch ? stripTags(titleMatch[1]) : htmlPath;
}

function loadReport() {
	const issues = [];

	for (const [fileOrder, htmlReport] of HTML_REPORTS.entries()) {
		if (!fs.existsSync(htmlReport))
			die(`HTML report not found: ${htmlReport}`);

		const reportHtml = fs.readFileSync(htmlReport, "utf8");
		const sourcePath = parseSourcePath(reportHtml, htmlReport);

		for (const issue of parseIssues(reportHtml, sourcePath))
			issues.push({...issue, fileOrder});
	}

	issues.sort((a, b) => {
		if (a.fileOrder !== b.fileOrder)
			return a.fileOrder - b.fileOrder;
		return a.position - b.position;
	});

	const elements = issues.map((issue, index) => new IssueElement(issue, index));
	const controlJs = findControlJs(HTML_REPORTS[0]);
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
		process.stdout.write("No uncovered line, region, or branch target found.\n");
		return;
	}

	const issue = element.issue;
	const source = `${issue.sourcePath}:${issue.line}` +
		(issue.column ? `:${issue.column}` : "");

	process.stdout.write(`Next uncovered ${issue.type} (${element.index + 1})
  source: ${source}
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
		process.stdout.write("No uncovered line, region, or branch target found.\n");
		return;
	}

	const counts = {branch: 0, line: 0, region: 0};

	for (const element of elements)
		counts[element.issue.type]++;

	process.stdout.write(
		`Uncovered issues: ${elements.length}` +
		` (lines=${counts.line}, regions=${counts.region}, ` +
		`branches=${counts.branch})\n`
	);

	for (const element of elements) {
		const issue = element.issue;
		const source = `${issue.sourcePath}:${issue.line}` +
			(issue.column ? `:${issue.column}` : "");

		process.stdout.write(`\nUncovered ${issue.type} (${element.index + 1})
  source: ${source}
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
