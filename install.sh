#!/bin/sh
# install.sh — build and install cljc from a source checkout.
#
#   ./install.sh                 # install to ~/.local (no sudo)
#   PREFIX=/usr/local ./install.sh   # system-wide (may need sudo)
#
# Requires: cc (gcc or clang), make. That's the whole dependency list.
set -e

PREFIX="${PREFIX:-$HOME/.local}"

if ! command -v cc >/dev/null 2>&1; then
    echo "error: no C compiler found (install gcc or clang)" >&2
    exit 1
fi

cd "$(dirname "$0")"

echo "building cljc (PREFIX=$PREFIX)..."
make clean >/dev/null 2>&1 || true
make PREFIX="$PREFIX"

echo "running test suite..."
make test

make PREFIX="$PREFIX" install

echo
echo "cljc installed."
echo "  binary:    $PREFIX/bin/cljc"
echo "  batteries: $PREFIX/share/cljc  (load-file/require find them anywhere)"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "  NOTE: $PREFIX/bin is not on your PATH" ;;
esac
"$PREFIX/bin/cljc" --version
