#!/bin/sh
# Compila o benchmark p57 e os shaders dele. Uso: tools/probe/p57/build.sh [saida_dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
for s in "$HERE"/*.vert "$HERE"/*.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$OUT/$(basename "$s").spv" "$s" >/dev/null
done
clang -O2 -Wall -o "$OUT/p57" "$HERE/../p57.c" -ldl -lm
echo "p57 -> $OUT"
