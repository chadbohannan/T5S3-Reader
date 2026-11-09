# Engineering Wiki — Schema & Operating Rules

This is a persistent, LLM-maintained knowledge base for capturing complex system behavior, incident patterns, debugging knowledge, and operational wisdom about a codebase. The LLM writes and maintains all wiki content. The human curates sources, directs investigation, and asks questions.

## Project

This wiki covers **T5S3 Reader**: firmware for the LilyGo T5S3 and EPD47 ESP32-S3 4.7-inch e-paper readers, adapted from CrossPoint Reader. The tracked code is the parent repository, listed in `repos.txt` (machine-local, gitignored). The `code-rag` glob is `T5S3*`. Filtering: this is the only wiki on the project, so ingest everything relevant.

## Directory Layout

```
.
├── CLAUDE.md          # This file — schema and operating rules
├── sources/           # Raw, immutable source documents (the LLM never modifies these)
├── wiki/              # LLM-generated and LLM-maintained markdown pages
│   ├── index.md       # Content catalog — the structured routing layer
│   ├── log.md         # Chronological record of all operations
│   ├── systems/       # Distinct systems, services, and deployments
│   ├── incidents/     # Specific incidents, outages, and postmortems
│   ├── components/    # Individual components, libraries, and dependencies
│   ├── concepts/      # Recurring patterns, failure modes, and architectural concepts
│   ├── runbooks/      # Operational procedures synthesized from incident history
│   ├── syntheses/     # Cross-cutting analyses, comparisons, and thematic summaries
│   └── planned/       # TODO / planned work: problem, evidence, plan, verification, status
└── scripts/           # Helper scripts for processing sources and pulling code updates (see scripts/README.md)
```

`sources/` holds inputs verbatim — postmortems, design docs, Slack threads, log dumps. The LLM reads them but never edits them. Everything under `wiki/` is LLM-authored and LLM-maintained.

## Page Conventions

Wiki pages are prose documents. They have no YAML frontmatter. The directory a page lives in determines its category. The first heading serves as the title.

Internal links use relative paths and are embedded in context — every link should appear inside a sentence that explains why the relationship matters. A link to another page is a claim about a connection, and the surrounding prose is the evidence.

```markdown
# Page Title

Opening paragraph establishing what this page covers and why it matters.

Body paragraphs with contextual links woven in. For example: "Under high
task throughput, the metrics exporter scales up and creates additional
connections through the [connection pool](../components/connection-pool.md),
which has a hard ceiling of 200 backend connections."
```

Source attribution is inline. When a factual claim traces to a specific source, cite it naturally: "The March 2026 design spec (`20260316-spec-queue-delivery-semantics.md`) evaluated three options..." or "According to the deploy operational-architecture documentation..." This gives citations semantic context — the reader understands not just *what* the source is, but *what claim it supports and why it was consulted.*

### Naming Conventions

- **Systems**: `{system-name}.md`
- **Incidents**: `{YYYY-MM-DD}-{short-description}.md`
- **Components**: `{component-name}.md`
- **Concepts**: `{concept-name}.md`
- **Runbooks**: `{procedure-name}.md`
- **Syntheses**: `{analysis-topic}.md`
- **Planned**: `{work-item}.md` — open with a bold `**Status: ...**` line (planned, in progress, done in `<sha>`, dropped); when the work lands, move the lasting facts into the component/concept pages and leave the planned page as a short record that links to them

Filenames are lowercase, hyphenated, and descriptive.

### Writing Style

Every sentence should earn its place. A short, precise page is better than a long, vague one. Write in plain, humanist prose — not corporate-memo bullet lists.

For incidents, open with a plain-English summary that captures severity, duration, affected services, and resolution in a sentence or two — "A SEV-2 incident lasting approximately 25 minutes affecting all production clusters, self-resolved when cloud capacity freed up." This conveys the same structured information as YAML fields but in a form that a reader (human or LLM) can absorb without parsing metadata.

Cross-references are a first-class feature. When creating or updating a page, consider what other pages should link here, and what this page should link to. But every link must be contextual — no "see also" lists at the bottom. If a relationship can't be explained in a sentence, it probably isn't meaningful enough to link.

Every concrete claim — a field name, a file path, a migration number, an SLA — is implicitly anchored either to current code (in which case sync must keep it true) or to a named source document (in which case the prose must say so: *"the PROJ-2342 RFD proposes..."* rather than *"the service retries tasks..."*). Before extending or relying on a code-anchored citation in an existing page, verify it still resolves.

When new information contradicts existing wiki content, don't silently overwrite. Note the contradiction explicitly, cite both sources, and flag it for the user. If a claim comes from a single source or is speculative, say so.

### Density and Single Ownership

Maximize information per token — *information*, not text. Cut any token the reader can already see and that can't differ from it: a page in `incidents/` needs no "Category: incident" line. Keep anything the reader can't reconstruct from what's in front of them, even if another layer also holds it.

Git records *edit*-time, invisible to anyone reading rendered markdown. So event dates live in the prose, and when a sync confirms a page's code-anchored claims, record verify-time: "synced against `<repo>`@`a1b2c3d` as of 2026-06." A page that says "the TTL was raised from 120s to 6h in June 2026 when the Helm chart was introduced" is more useful than one that just says "the TTL is 6h" — the history is the part the code can't tell you.

Links stay inline because that's denser: "scales connections through the [Thread Pool](../components/thread-pool.md), which caps at 200" carries the relationship *and* the constraint — a `Related:` footer carries neither.

Duplicate *immutable* facts freely (a past incident's date will never change). Give every *mutable* fact — a config value, a threshold, a cap — one owning page and link to it; each copy is one more place a sync must correct.

## The Index

`wiki/index.md` is the structured routing layer for the wiki. It is the first thing an LLM reads when answering a query, and the primary mechanism for navigating the wiki at scale.

Each entry is a link with a one-line description. Entries are grouped by category. For incidents, the description should include severity and the affected system so that an LLM (or a script) can triage without opening the page.

```markdown
# Wiki Index

## Systems
- [System Name](systems/system-name.md) — one-line description

## Incidents
- [2026-01-09: API OOM Kill](incidents/2026-01-09-api-oom.md) — SEV-2, all 6 API pods OOMKilled due to memory request misconfiguration

## Components
- [Component Name](components/component-name.md) — one-line description

## Concepts
- [Concept Name](concepts/concept-name.md) — one-line description

## Runbooks
- [Procedure Name](runbooks/procedure-name.md) — one-line description

## Syntheses
- [Analysis Topic](syntheses/analysis-topic.md) — one-line description

## Planned
- [Work Item](planned/work-item.md) — status, one-line description
```

The index is updated on every ingest. As the wiki scales, the index may grow to include additional structured hints (severity tags, component names, date ranges) to support pre-filtering — but this is done in the index itself, not in individual pages. The index links only to wiki pages; do not clutter it with links to raw `sources/` material.

## The Log

`wiki/log.md` is a chronological, append-only record of wiki operations. Each entry is a single line with a consistent prefix for parseability:

```
[YYYY-MM-DD] operation | Subject — brief description of what happened
```

For ingest operations, the subject should be a relative link to the source document (`[name](../sources/source-file.md)`). Other mutation operations use plain-text subjects.

Operations: `ingest`, `query`, `lint`, `enrich`, `sync`, `update`.

## Discovery Layers

This wiki is fundamentally *about* a codebase, so wiki pages should describe code behavior, fields, surfaces, and configuration directly. The wiki is what an on-call engineer reads when paging through an incident at 2am; the code is the ground truth they reference once they know where to look. The two are complementary, not exclusive.

- **The wiki** is the narrative and historical layer over the code: how components fit together, what their fields mean, how they fail, how they're operated, what changed and why and when. Incident stories need accurate code-level detail to be useful, so don't hold back from describing schemas, status fields, retry behavior, API surfaces, deploy procedures, resource configurations, or version history on the relevant pages. Be dense with concrete detail: dates, commit ranges, configuration values, file paths, function names, thresholds, resource limits, migration numbers.
- **The code** is ground truth. Reach into it two ways:
  - **`code-rag`** (optional) — a semantic code-search service that indexes repositories and surfaces function signatures, config files, and implementation details. If available, scope searches to this project with `T5S3*` filter globs (the indexed corpus usually spans many unrelated repos). The CLI client is `scripts/code-rag-cli.py` — see [scripts/README.md](scripts/README.md).
  - **Local clones** — `grep`/`glob`/`read` over the repositories listed in `repos.txt`. Always available, always current to the local checkout. Use this when there's no code-rag instance, or to read a file in full once a search points at it.
- **`index.md`** is the routing layer. An LLM reads the index to find relevant wiki pages, then drills into source code (via code-rag or the local clones) when the wiki points toward a specific component or behavior.

During enrichment, code searches should be used liberally to find implementation details that strengthen wiki pages. Findings are integrated as prose with citations to the specific code paths.

**Bias toward ingesting.** If a change, source, or commit relates to T5S3 Reader, default to capturing it somewhere — even a one-line update to a component page is better than skipping it because it "isn't incident-grade" or "is just code." The cost of a small, accurate update is low; the cost of a missing fact during an incident is high.

## Operations

### Ingest

When the user provides a new source (a postmortem, debug log, code snippet, architecture doc, Slack thread, commit history, etc.):

1. **Read** the source fully.
2. **Create or update** wiki pages:
   - Use Simplified Technical English
   - Create or update entity pages for every system, component, or concept mentioned.
   - Create or update incident pages if the source describes an incident.
   - Extract any operational procedures into runbook pages.
   - Weave cross-references into the prose of every page touched — both the new/updated pages and existing pages that should now link to them.
   - Split larger files on topical boundaries to optimize lookup via index.md; large files consume context.
3. **Update index.md** — add or revise entries for every wiki page touched.
4. **Append to log.md** — a temporal index, no more than 400 charachters recording the ingest.
5. **Report** to the user: what pages were created/updated, what connections were found, what gaps remain (missing data or opportunities to lint).

The steps above describe ingesting one source interactively. To ingest in bulk — a whole directory of sources, each in its own headless pass — use `scripts/ingest-folder.sh` (or `ingest-tree.sh` to recurse and glob-filter); both can skip sources already recorded in `log.md`. See [scripts/README.md](scripts/README.md) for invocation and flags.

### Query

When the user asks a question:

1. **Read index.md** first to identify relevant pages.
2. **Read** the relevant wiki pages.
3. **Use code search** (code-rag or local clones) if the question needs grounding in source code.
4. **Synthesize** an answer with citations to wiki pages and original sources.
5. If the answer is substantial and reusable, **offer to file it** as a new page.
6. **Log** the query.

When the user asks for help reviewing a merge/pull request:

1. **Verify the repos are up to date** so you have a view of current content — run the sync pull (see [Sync](#sync)).
2. **Pull the default branch.** There should never be unstashed changes on `main`/`master`, so always do this before reading the diff.
3. **Ingest the diff.** The state of the system has changed, so pull that knowledge into the wiki, then lint and refine.
4. **Support the user** — be more interactive than autonomous here; surface what the change touches and let the user steer.

### Lint

When the user asks for a health check (or periodically when the wiki has grown):

1. Check for **contradictions** between pages.
2. Find **orphan pages** — pages not linked from any other page (`index.md` links don't count).
3. Identify **mentioned-but-missing** entities that deserve their own page.
4. Flag **stale pages** — prefer a page's recorded verify-time stamp where it has one; fall back to `git log` for pages without one (not updated in a long time relative to their subject matter). Note that git's edit-time is a weaker signal: a typo fix bumps it without re-verifying anything.
5. Verify **link integrity** — all internal links resolve to existing files.
6. Suggest **new questions** or sources that would fill gaps.
7. Report findings and fix what can be fixed automatically.
8. **Log** the lint pass.

The structural checks (broken links, orphans, reference counts) are automated by `scripts/lint-wiki.py`. Run it first, then handle the prose-level checks (contradictions, missing entities, staleness) by reading.

### Enrich

When deeper context could improve clarity:

1. Search indexed repositories (code-rag, scoped with `T5S3*`) or the local clones for relevant source code, architecture patterns, and implementation details.
2. Integrate findings into wiki pages as prose with citations to specific code paths.
3. Use `grep`/`glob`/`read` on local repositories when a search points to specific files worth reading in full.
4. **Log** the enrichment.

### Sync

When the user asks to sync the wiki against recent code changes (or when a query depends on whether the local clones reflect what's in production):

1. Run `scripts/pull-repos.sh`. It reads `repos.txt` from the repo root (one absolute repo path per line), fast-forwards `main`/`master` for each, skips any repo on a feature branch, and prints one line per repo that moved in the form `repo before..after`.
2. For each repo that moved, run `git -C <repo-path> log --oneline <before>..<after>` to inspect the new commits. Read the diffs for anything T5S3 Reader-related — new fields, behavior changes, config knobs, schema changes, deploy procedures, runbook updates, alert thresholds, terminology shifts. Default to capturing rather than skipping.
3. **Hunt for broken anchors.** For each diff, grep the wiki for the field names, file paths, function names, or migration numbers it touches. Existing claims about those surfaces are the primary signal of sync-relevance — finding one means the wiki may be wrong, not just incomplete.
4. **Discuss** with the user which changes are wiki-relevant before writing anything. The bar is low: if a change adds or alters a real surface (an API field, a status value, a config option, a procedure, a documented behavior), update the relevant component or concept page. Pure refactors, test-only changes, and cosmetic doc tweaks can be skipped — but err toward including, not excluding.
5. **Update** affected wiki pages, weaving cross-references and citing specific commits or files. When a page's code-anchored claims are confirmed still true against the synced HEAD, record verify-time in the prose ("synced against `<repo>`@`<sha>` as of `<YYYY-MM>`") so freshness survives in the page, not just in git.
6. **Note staleness**: if you use code-rag, its index is refreshed separately and may lag behind the local clones. If a sync surfaces material changes that code-rag would still describe out-of-date, mention it so the user can decide whether to re-embed.
7. **Append to log.md** — one entry per sync, noting the commit range(s) and which pages were touched.

## Incident Page Structure

Incident pages don't use a rigid template, but they should cover these aspects in whatever order makes the narrative clearest:

- **Opening summary**: severity, duration, affected services, and outcome in one or two sentences.
- **Timeline**: what happened and when.
- **Root cause**: what actually broke and why.
- **Detection**: how it was noticed — alerts, customer reports, internal observation.
- **Resolution**: what fixed it.
- **Contributing factors**: systemic issues that made this incident possible or worse, with links to relevant concept and component pages.
- **Patterns**: connections to other incidents and recurring failure modes, woven into the narrative rather than listed at the end.
