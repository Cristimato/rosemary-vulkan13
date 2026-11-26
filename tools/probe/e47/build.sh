#!/bin/sh
# Compila o probe e47 e os shaders dele. Uso: tools/probe/e47/build.sh [saida_dir]
# Rodar no aparelho como root:
#   export LD_LIBRARY_PATH=/vendor/lib64/hw:/vendor/lib64:/vendor/lib64/mt6785
#   <dir>/e47 /vendor/lib64/hw/vulkan.mt6785.so <dir>        # vkshim (instalado)
#   <dir>/e47 /vendor/lib64/hw/vulkan.r49.so <dir>           # driver puro (mostra os bugs)
#   E47_BENCH=1 <dir>/e47 <hal> <dir>                        # custo de CPU por chamada
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
for s in "$HERE"/*.vert "$HERE"/*.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$OUT/$(basename "$s").spv" "$s" >/dev/null
done
clang -O2 -Wall -o "$OUT/e47" "$HERE/../e47.c" -ldl -lpthread
echo "e47 -> $OUT"
