#!/usr/bin/env bash
#
# Fast-forward the code repositories this wiki tracks, and report which moved.
#
# Reads repo paths from repos.txt at the repo root (one absolute path per line;
# blank lines and lines starting with '#' are ignored). Override the list file
# with REPOS_FILE, or point at a parent directory of sibling repos with
# REPOS_DIR (every immediate child that is a git repo is pulled).
#
# For each repo: skips it if checked out on a feature branch, otherwise runs
# `git pull --ff-only`. Prints one line per repo whose HEAD moved, in the form
#   repo before..after
# which the Sync operation in CLAUDE.md feeds into `git log --oneline`.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
REPOS_FILE="${REPOS_FILE:-$root/repos.txt}"

# Build the list of repo directories.
repos=()
if [[ -n "${REPOS_DIR:-}" ]]; then
    for dir in "$REPOS_DIR"/*/; do
        [ -d "$dir/.git" ] && repos+=("${dir%/}")
    done
elif [[ -f "$REPOS_FILE" ]]; then
    while IFS= read -r line; do
        line="${line%%#*}"                       # strip trailing comments
        line="$(echo "$line" | xargs 2>/dev/null || true)"  # trim whitespace
        [[ -z "$line" ]] && continue
        repos+=("$line")
    done < "$REPOS_FILE"
else
    echo "no repos configured: create $REPOS_FILE or set REPOS_DIR" >&2
    exit 1
fi

for dir in "${repos[@]}"; do
    repo=$(basename "$dir")

    if [[ ! -d "$dir/.git" ]]; then
        echo "skipping $repo (not a git repo: $dir)" >&2
        continue
    fi

    branch=$(git -C "$dir" symbolic-ref --short HEAD 2>/dev/null || true)
    if [[ "$branch" != "main" && "$branch" != "master" ]]; then
        echo "skipping $repo (on $branch)" >&2
        continue
    fi

    before=$(git -C "$dir" rev-parse HEAD)
    if ! git -C "$dir" pull --ff-only --quiet 2>/dev/null; then
        echo "pull failed for $repo" >&2
        continue
    fi
    after=$(git -C "$dir" rev-parse HEAD)

    if [[ "$before" != "$after" ]]; then
        echo "$repo $before..$after"
    fi
done
