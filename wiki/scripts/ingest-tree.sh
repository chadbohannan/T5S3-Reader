#!/usr/bin/env bash
#
# Recursively ingest files matching a glob under a directory tree, one
# `claude` invocation per file. Like ingest-folder.sh but walks subdirectories
# and filters by filename pattern.
#
# Usage: ingest-tree.sh [--dry-run] [--skip-ingested] <source-directory> [glob-pattern]
#   --dry-run        print the claude commands instead of running them
#   --skip-ingested  skip files already recorded as ingested in wiki/log.md
#   glob-pattern     filename pattern (default: '*'), e.g. '*.md'.
#                    MUST be quoted, or the shell expands it before the script runs.
#
# Example: ingest-tree.sh ./sources '*.md'   # note the quotes
#
set -euo pipefail

dry_run=false
skip_ingested=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) dry_run=true; shift ;;
    --skip-ingested) skip_ingested=true; shift ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *) break ;;
  esac
done

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: ingest-tree [--dry-run] [--skip-ingested] <source-directory> [glob-pattern]" >&2
  echo "Example: ingest-tree ./sources '*.md'" >&2
  echo "Note: quote the glob-pattern, or the shell expands it into extra arguments." >&2
  exit 1
fi

srcdir="$1"
pattern="${2:-*}"

if [[ ! -d "$srcdir" ]]; then
  echo "Error: '$srcdir' is not a directory" >&2
  exit 1
fi

logfile="wiki/log.md"

while IFS= read -r -d '' file; do
  filename="$(basename "$file")"

  if $skip_ingested && [[ -f "$logfile" ]]; then
    if grep -F "ingest |" "$logfile" | grep -qF "$filename"; then
      echo "--- Skipping (already ingested): $file ---"
      continue
    fi
  fi

  echo "--- Ingesting: $file ---"
  prompt="(Re)ingest '$file' following the ingest operation in CLAUDE.md. Use code search (code-rag or local clones) and source code to enrich the wiki where possible. Update index.md and append to log.md."
  if $dry_run; then
    echo claude -p "$prompt" --dangerously-skip-permissions
  else
    claude -p "$prompt" --dangerously-skip-permissions > /dev/null
  fi
done < <(find "$srcdir" -type f -name "$pattern" -print0 | sort -z)
