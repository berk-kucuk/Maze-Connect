#!/usr/bin/env bash
# Build the maze-connect Arch package from this working tree.
#
#   ./build.sh                 -> dist/maze-connect-<ver>-<rel>-<arch>.pkg.tar.zst
#   ./build.sh --install       -> build, then pacman -U the result
#   ./build.sh --repo <dir>    -> build, then add it to a local repo
#
# makepkg must run as a normal user, not root.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pkgver="$(sed -n 's/^pkgver=//p' "$here/packaging/PKGBUILD")"

# The version lives in two files, and they have drifted before: a bump to
# CMakeLists alone produced a package still labelled with the old version,
# which then refused to install over itself. Refuse to build rather than ship
# a package whose name disagrees with the binary inside it.
cmakever="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9.]\+\).*/\1/p' "$here/CMakeLists.txt" | head -1)"
if [ "$pkgver" != "$cmakever" ]; then
    echo "Version mismatch: PKGBUILD says $pkgver, CMakeLists.txt says $cmakever" >&2
    echo "Bump both before building." >&2
    exit 1
fi
pkgname="$(sed -n 's/^pkgname=//p' "$here/packaging/PKGBUILD")"

if [[ $EUID -eq 0 ]]; then
    echo "Run this as a normal user; makepkg refuses to run as root." >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Ship the sources makepkg needs and nothing else — build trees and the
# local git history would only bloat the tarball.
#
# packaging/ is NOT excluded, even though the PKGBUILD inside it is copied
# out separately below. It used to hold only that file; it now also holds
# maze-connect-status and icons/, both of which CMake installs. Excluding
# the directory made the build fail in package(), long after the tests had
# passed, with a missing-file error that pointed at the source tree rather
# than at this line.
staging="$work/$pkgname-$pkgver"
mkdir -p "$staging"
tar -c \
    --exclude='./build' --exclude='./build-*' --exclude='./.git' \
    --exclude='./dist' \
    -C "$here" . | tar -x -C "$staging"

tar -czf "$work/$pkgname-$pkgver.tar.gz" -C "$work" "$pkgname-$pkgver"
# The PKGBUILD and its install scriptlet sit beside the tarball, not inside
# it: makepkg reads both from its own working directory.
cp "$here/packaging/PKGBUILD" "$here/packaging/$pkgname.install" "$work/"

( cd "$work" && makepkg -f --noconfirm )

mkdir -p "$here/dist"
cp "$work"/*.pkg.tar.zst "$here/dist/"

# makepkg also emits a -debug package with the split symbols; the one to
# install or publish is the plain one.
built="$here/dist/$pkgname-$pkgver-$(sed -n 's/^pkgrel=//p' "$here/packaging/PKGBUILD")-$(uname -m).pkg.tar.zst"
echo "Built: $built"
ls -1 "$here/dist"/*.pkg.tar.zst

case "${1-}" in
    --install)
        sudo pacman -U --noconfirm "$built"
        ;;
    --repo)
        repo="${2:?--repo needs a directory}"
        mkdir -p "$repo"
        cp "$built" "$repo/"
        repo-add "$repo/$(basename "$repo").db.tar.gz" "$repo/$(basename "$built")"
        ;;
esac
