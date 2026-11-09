# T5S3 Reader Engineering Wiki

Inspired by: Andrej karpathy's LLM-Wiki https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f

A starter template for a persistent, LLM-maintained knowledge base about a codebase — capturing system behavior, incident patterns, debugging knowledge, and operational wisdom. The LLM writes and maintains everything under `wiki/`; you curate the raw inputs in `sources/`, direct investigation, and ask questions. The full schema and operating rules live in [CLAUDE.md](CLAUDE.md), which the LLM reads as its standing contract.

## Setup

Bootstrapped from [code-wiki-template](https://github.com/chadbohannan/code-wiki-template). `repos.txt` (gitignored) points at the firmware checkout one directory up. Drop sources into `sources/` and ask the agent to ingest them. Pages live in `wiki/` (so `wiki/wiki/` from the repo root).

## Daily use

Talk to the agent in terms of the operations defined in CLAUDE.md: **ingest** a new source, **query** the wiki, **sync** against recent code changes, **lint** for health, or **enrich** a page from code. Helper scripts for bulk ingest, syncing repos, and linting are in `scripts/` — see [scripts/README.md](scripts/README.md).
