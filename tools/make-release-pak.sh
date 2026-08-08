#!/usr/bin/env sh
# Assembles the release-pak: a pre-built binary zip of the release game, cleaned
# of the packager's personal data. The recipient only unzips and runs run.sh
# (macOS/Linux) or run.bat (Windows) — no source, no build, no compiler. The
# recipient's config/options.properties and saves/ are created on their own
# machine at first run, so nothing personal (logs, worlds, options) is shipped.
#
# Usage: ./tools/make-release-pak.sh
# Output: build/mc-rebedrock-<version>-win-mac.zip
set -e
cd "$(dirname "$0")/.."

GAME="build/rebedrock-release/game"
if [ ! -x "$GAME/bin/mc_rebedrock" ] || [ ! -f "$GAME/bin/mc_rebedrock.exe" ]; then
    echo "Release game is missing one or both binaries (macOS + Windows)." >&2
    echo "Build the release branch first: ./build.sh" >&2
    exit 1
fi

VERSION="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}' || echo 0.1.0)"
PAK="build/release-pak"
ZIP="build/mc-rebedrock-${VERSION}-win-mac.zip"
rm -rf "$PAK" "$ZIP"
mkdir -p "$PAK"/bin "$PAK"/resources "$PAK"/config "$PAK"/saves

cp "$GAME/bin/mc_rebedrock" "$GAME/bin/mc_rebedrock.exe" "$PAK/bin/"
cp -R "$GAME/resources/." "$PAK/resources/"
cp tools/pak/run.sh tools/pak/run.bat "$PAK/"
chmod +x "$PAK/run.sh" "$PAK/bin/mc_rebedrock"
find "$PAK" -name ".DS_Store" -delete

cat > "$PAK/README.txt" <<'EOF'
mc-rebedrock (pre-built binaries)

Run run.bat on Windows or run.sh on macOS/Linux to launch. Nothing needs to be
compiled or installed beyond unzipping this folder. Your own
config/options.properties and saves/ are created here on the first run.
EOF

if command -v zip >/dev/null 2>&1; then
    ( cd build && zip -r -q "$(basename "$ZIP")" "$(basename "$PAK")" )
    echo "Release pak: $ZIP"
else
    echo "zip not found; left the assembled bundle at $PAK (run.sh / run.bat inside)."
fi
