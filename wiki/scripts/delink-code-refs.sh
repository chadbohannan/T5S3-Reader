#!/usr/bin/env bash
# Convert markdown hyperlinks to .go files into inline code spans.
# [controller.go:826](../path/to/controller.go#L826) → `controller.go:826`
# Only operates on files under sources/.

set -euo pipefail

SOURCES_DIR="${1:-sources}"

if [[ ! -d "$SOURCES_DIR" ]]; then
  echo "Usage: $0 [sources-dir]" >&2
  exit 1
fi

find "$SOURCES_DIR" -name '*.md' -exec grep -l '\.go' {} \; | while read -r f; do
  sed -i '' -E 's/\[([^]]+)\]\(([^):]*\.go[^)]*)\)/`\1`/g' "$f"
done

echo "Done. Converted .go hyperlinks to inline code spans."
