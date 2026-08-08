#!/usr/bin/env sh
# mc-rebedrock (pre-built binary) — macOS / Linux launcher.
# Just runs the bundled game; there is no build step. Your own
# config/options.properties and saves/ are created inside this folder on first
# run, so nothing is shared with the packager.
set -e
cd "$(dirname "$0")"
exec ./bin/mc_rebedrock "$@"
