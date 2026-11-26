#!/bin/sh
# Compila o vkshim v4 (HAL wrapper Vulkan + passes SPIR-V Mali) para arm64 Android.
# Uso: shim/build.sh [saida] [module|portable]
#   module   (padrão): HAL do módulo; abre /vendor/lib64/hw/vulkan.pixelbc.so e vulkan.r49.so
#   portable: pacote importável; procura vulkan.pixelbc.so / vulkan.r49.so na pasta do pacote
# Requer tools/build_deps.sh (SPIRV-Tools do fork em deps/).
set -e
cd "$(dirname "$0")"
OUT=${1:-../out/vulkan.mt6785.so}
MODE=${2:-module}
SPVT=${SPVT:-../deps/SPIRV-Tools}
SONAME=vulkan.mt6785.so; DEFS=
if [ "$MODE" = portable ]; then DEFS=-DPORTABLE; SONAME=$(basename "$OUT"); fi
O=$(mktemp -d)
CF="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections"
clang   $CF -Wall -Wno-unused-result -DBCN_DEFAULT=${BCN_DEFAULT:-1} $DEFS -c shim.c -o "$O/shim.o"
clang   $CF -Wall -c clipcull.c -o "$O/clipcull.o"
clang++ $CF -std=c++17 -fvisibility-inlines-hidden -I"$SPVT/include" -c spvfix.cpp -o "$O/spvfix.o"
mkdir -p "$(dirname "$OUT")"
clang++ -shared -o "$OUT" "$O/shim.o" "$O/clipcull.o" "$O/spvfix.o" \
  "$SPVT/build/source/opt/libSPIRV-Tools-opt.a" "$SPVT/build/source/libSPIRV-Tools.a" \
  -static-libstdc++ -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--no-undefined \
  -Wl,-soname,"$SONAME" -Wl,-z,max-page-size=16384 -ldl -llog
rm -rf "$O"
llvm-strip --strip-unneeded "$OUT"
patchelf --remove-rpath "$OUT"     # o RUNPATH do Termux não existe no namespace sphal
echo "vkshim v4 ($MODE) -> $OUT"
