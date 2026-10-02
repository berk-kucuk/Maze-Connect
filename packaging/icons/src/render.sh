#!/usr/bin/env bash
# Regenerates every Maze Connect icon from gen_icons.py.
#   desktop: packaging/icons/        (hicolor PNGs 16-512, scalable SVG, mark)
#   android: ../../../../Maze-Connect-Mobile (legacy mipmaps, store icon)
# The Android adaptive layers and the notification glyph are vector drawables
# kept in the mobile repo; they use the same 1024 grid (see their comments).
# Needs: python3, rsvg-convert.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
icons=$(dirname "$here")
mobile=$(cd "$here/../../../../Maze-Connect-Mobile" 2>/dev/null && pwd || true)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$here/gen_icons.py" "$tmp"

png() { rsvg-convert -w "$2" -h "$2" "$tmp/$1" -o "$3"; }

# Desktop. 16-32 use the reduced mark, as Maze AI and Qlam do at tray size.
for s in 16 22 24 32; do png maze-connect-small.svg "$s" "$icons/maze-connect-$s.png"; done
for s in 48 64 128 256 512; do png maze-connect.svg "$s" "$icons/maze-connect-$s.png"; done
png maze-connect.svg 1024 "$icons/maze-connect.png"
png maze-connect-mark.svg 1024 "$icons/maze-connect-mark.png"
cp "$tmp/maze-connect.svg" "$icons/maze-connect.svg"

# Android: legacy launcher PNGs (pre-26 launchers) and the Play store icon.
if [ -n "$mobile" ]; then
    res="$mobile/app/src/main/res"
    for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
        png maze-connect.svg "${d#*:}" "$res/mipmap-${d%%:*}/ic_launcher.png"
        png maze-connect-round.svg "${d#*:}" "$res/mipmap-${d%%:*}/ic_launcher_round.png"
    done
    png maze-connect-store.svg 512 "$mobile/fastlane/metadata/android/en-US/images/icon.png"
fi
