#!/bin/sh
# Compila l'encoder MP3 LAME 3.100 (LGPL) come eseguibile universale (arm64 + x86_64) in third_party/bin/lame.
# L'app lo usa come programma separato per l'esportazione in MP3 (nessun collegamento alla libreria).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/bin/lame"
[ -x "$OUT" ] && [ "$1" != "-f" ] && { echo "lame gia' compilato: $OUT"; exit 0; }
echo "ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e  $ROOT/third_party/lame-3.100.tar.gz" | shasum -a 256 -c -
WORK="$(mktemp -d)"
tar -xzf "$ROOT/third_party/lame-3.100.tar.gz" -C "$WORK"
cd "$WORK/lame-3.100"
ARCHS="${LAME_ARCHS:--arch arm64 -arch x86_64}"
./configure --disable-shared --enable-static --disable-dependency-tracking --disable-gtktest \
    CFLAGS="$ARCHS -O2 -mmacosx-version-min=11.0" LDFLAGS="$ARCHS -mmacosx-version-min=11.0" >/dev/null
make -j4 >/dev/null
mkdir -p "$(dirname "$OUT")"
cp frontend/lame "$OUT"
cd "$ROOT"; rm -rf "$WORK"
lipo -info "$OUT"
