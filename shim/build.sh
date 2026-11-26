#!/bin/sh
# Compila o vkshim (HAL wrapper Vulkan) para arm64 Android.
# Uso: shim/build.sh [saida]   (padrao: out/vulkan.mt6785.so)
# Requer: clang com NDK/bionic (Termux serve), headers Vulkan, patchelf.
set -e
cd "$(dirname "$0")"
OUT=${1:-../out/vulkan.mt6785.so}
mkdir -p "$(dirname "$OUT")"
clang -shared -fPIC -O2 -Wall -Wno-unused-result \
  -Wl,-soname,vulkan.mt6785.so -Wl,--no-undefined \
  -o "$OUT" shim.c -ldl -llog
# o RUNPATH do Termux nao existe no namespace sphal
patchelf --remove-rpath "$OUT"
echo "vkshim -> $OUT"
