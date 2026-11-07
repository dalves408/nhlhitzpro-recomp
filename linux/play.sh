#!/bin/sh
# Dev-tree launcher (installer packages its own play.sh). Prefer scripts/play-linux.sh.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/scripts/play-linux.sh" "$@"
