# Wiki Helper Scripts

Tooling that supports the operations defined in [`../CLAUDE.md`](../CLAUDE.md). The scripts are deliberately stdlib-only (bash + Python 3, no third-party packages) so the template runs anywhere.

## Configuration

Before first use, copy `repos.txt.example` (at the repo root) to `repos.txt` and list the absolute path of every code repository this wiki tracks — one per line. `pull-repos.sh` reads this file; nothing else needs it. `repos.txt` holds machine-local paths and is gitignored — project *identity* (name, description, code-rag glob) lives committed in [`../CLAUDE.md`](../CLAUDE.md) instead, and the code-rag glob usually matches your `repos.txt` basenames.

## Scripts

| Script | What it does | Operation |
|--------|--------------|-----------|
| `pull-repos.sh` | Fast-forwards each tracked repo's `main`/`master`, skips feature branches, prints `repo before..after` for any that moved. Reads `repos.txt` (or `REPOS_DIR`/`REPOS_FILE` env overrides). | Sync |
| `lint-wiki.py` | Structural lint: broken internal links, orphan pages, and reference counts. Does **not** check contradictions or staleness — those need prose reading. Exit code 1 if broken links found. | Lint |
| `code-rag-cli.py` | Stdlib CLI client for a `code-rag` semantic code-search server (`search`, `unit`, `fetch`, `units`, `files`, `repos`, `status`, `browse`, `index`...). Plain-text output for LLM consumption. Optional — only needed if you run a code-rag instance. | Query / Enrich |
| `ingest-folder.sh` | Batch-ingests every file in a directory, one headless `claude -p` per file running the Ingest operation. `--skip-ingested` skips files already in `log.md`. | Ingest |
| `ingest-tree.sh` | Like `ingest-folder.sh` but recurses subdirectories and filters by glob (quote the glob: `'*.md'`). `--dry-run` prints commands; `--skip-ingested` skips done files. | Ingest |
| `delink-code-refs.sh` | Cleanup: rewrites markdown hyperlinks to `.go` files under `sources/` into inline code spans (`[controller.go:826](...)` → `` `controller.go:826` ``). Adapt the extension for other languages. | Maintenance |

`code-rag-cli.py` is only a client. The server it talks to is [`py-mcp-code-rag`](https://github.com/chadbohannan/py-mcp-code-rag) — stand up an instance there if you want semantic code search; otherwise the wiki falls back to `grep`/`glob`/`read` over the repos in `repos.txt`.

## Usage examples

```bash
# Sync: pull tracked repos and see what moved
scripts/pull-repos.sh

# Lint: structural health check over wiki/
scripts/lint-wiki.py wiki

# Ingest a whole folder of sources, skipping anything already done
scripts/ingest-folder.sh --skip-ingested sources/

# code-rag search scoped to this project (if you run a code-rag server)
scripts/code-rag-cli.py search "connection pool ceiling" --glob 'myproj*'
```

## Project-specific scripts

- `sim-txt-index.py <file.txt>...` — host simulation of the TXT reader's page indexer (`TxtReaderActivity::loadPageAtOffset`); counts glyph measurements, allocations and SD reads. Evidence for [TXT indexing performance](../wiki/planned/txt-indexing-performance.md).

Operational tooling tied to a specific codebase (one-off bulk-remediation scripts, cluster query wrappers, etc.) is welcome here too — keep it alongside these generic helpers. The template ships only the project-agnostic core.
