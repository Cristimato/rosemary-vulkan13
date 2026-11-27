#!/bin/sh
# Compila o probe c92 e os shaders dele. Uso: tools/probe/c92/build.sh [saida_dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
for s in "$HERE"/*.comp "$HERE"/*.vert "$HERE"/*.frag; do
  [ -f "$s" ] || continue
  glslangValidator -V --target-env vulkan1.2 -o "$OUT/$(basename "$s").spv" "$s" >/dev/null
done
clang -O2 -Wall -o "$OUT/c92" "$HERE/../c92.c" -ldl
echo "c92 -> $OUT"
