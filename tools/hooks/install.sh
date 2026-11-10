#!/usr/bin/env bash
# Points git at the repo's hooks directory.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath tools/hooks
echo "git hooks installed (core.hooksPath = tools/hooks)"
