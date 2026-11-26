#!/bin/sh
# Compila o vkshim (HAL wrapper Vulkan) para arm64 Android.
# Uso: shim/build.sh [saida] [portable]
#   padrao:   out/vulkan.mt6785.so  (modulo KSU: abre /vendor/lib64/hw/vulkan.r49.so)
#   portable: carrega vulkan.r49.so da pasta do proprio driver (pacote importavel)
# Requer: clang com NDK/bionic (Termux serve), headers Vulkan, patchelf.
set -e
cd "$(dirname "$0")"
OUT=${1:-../out/vulkan.mt6785.so}
SONAME=vulkan.mt6785.so
DEFS=
if [ "$2" = portable ]; then DEFS=-DPORTABLE; SONAME=$(basename "$OUT"); fi
mkdir -p "$(dirname "$OUT")"
clang -shared -fPIC -O2 -Wall -Wno-unused-result $DEFS \
  -Wl,-soname,$SONAME -Wl,--no-undefined \
  -o "$OUT" shim.c -ldl -llog
# o RUNPATH do Termux nao existe no namespace sphal
patchelf --remove-rpath "$OUT"
echo "vkshim${DEFS:+ (portatil)} -> $OUT"
