#!/usr/bin/env bash
#
# Batch-ingest every file in a directory by invoking `claude` once per file,
# each running the Ingest operation from CLAUDE.md headlessly.
#
# Usage: ingest-folder.sh [--skip-ingested] <source-directory>
#   --skip-ingested  skip files already recorded as ingested in wiki/log.md
#
set -euo pipefail

skip_ingested=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-ingested) skip_ingested=true; shift ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *) break ;;
  esac
done

if [[ $# -ne 1 ]]; then
  echo "Usage: ingest-folder [--skip-ingested] <source-directory>" >&2
  exit 1
fi

srcdir="$1"

if [[ ! -d "$srcdir" ]]; then
  echo "Error: '$srcdir' is not a directory" >&2
  exit 1
fi

logfile="wiki/log.md"

for file in "$srcdir"/*; do
  [[ -f "$file" ]] || continue
  filename="$(basename "$file")"

  if $skip_ingested && [[ -f "$logfile" ]]; then
    if grep -F "ingest |" "$logfile" | grep -qF "sources/$filename"; then
      echo "--- Skipping (already ingested): $filename ---"
      continue
    fi
  fi

  echo "--- Ingesting: $filename ---"
  claude -p "Ingest 'sources/$filename' following the ingest operation in CLAUDE.md. Read the source fully, create or update wiki pages, discover relevant code (code-rag or local clones), weave cross-references into existing pages that should link to the new content, update index.md, and append the ingest to log.md." --dangerously-skip-permissions > /dev/null
done
