#!/bin/sh
# Baixa e compila as dependências do vkshim v4 em commits fixos (build reproduzível).
#   - leegao/SPIRV-Tools (fork com passes para o compilador Mali) -> bibliotecas estáticas
#   - supplefrog/PixelBC (texturas BC na GPU), variante sem perda, + patches/pixelbc-backend-path.patch
# Uso: tools/build_deps.sh            (saída em deps/)
# Requer: git, cmake, ninja, clang/clang++, glslangValidator, python3, patchelf
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=$ROOT/deps
JOBS=${JOBS:-6}
SPVT_COMMIT=20a5b4305f22d94f78281d7fd9a185ae89590d4f
SPVH_COMMIT=97e96f9e9defeb4bba3cfbd034dec516671dd7a3
PBC_COMMIT=e994ccf4b6ef2885ab032a4ca39b739211cfe161
mkdir -p "$DEPS"

fetch() { # dir url commit
  if [ ! -e "$1/.git" ]; then git init -q "$1" && git -C "$1" remote add origin "$2"; fi
  git -C "$1" fetch -q --depth 1 origin "$3" && git -C "$1" checkout -q --force "$3"
}

# --- SPIRV-Tools (fork leegao) ---
S=$DEPS/SPIRV-Tools
fetch "$S" https://github.com/leegao/SPIRV-Tools "$SPVT_COMMIT"
fetch "$S/external/spirv-headers" https://github.com/KhronosGroup/SPIRV-Headers "$SPVH_COMMIT"
if [ ! -f "$S/build/source/opt/libSPIRV-Tools-opt.a" ]; then
  cmake -S "$S" -B "$S/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_CXX_FLAGS="-fvisibility=hidden -fvisibility-inlines-hidden" >/dev/null
  ninja -C "$S/build" -j"$JOBS" SPIRV-Tools-static SPIRV-Tools-opt
fi

# --- PixelBC (lossless) ---
P=$DEPS/PixelBC
fetch "$P" https://github.com/supplefrog/PixelBC "$PBC_COMMIT"
git -C "$P" apply --check "$ROOT/patches/pixelbc-backend-path.patch" 2>/dev/null && git -C "$P" apply "$ROOT/patches/pixelbc-backend-path.patch"
build_pbc() { # builddir backend saida
  cmake -S "$P" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPVB_ETC2=OFF \
    -DVULKAN_HEADERS_DIR="${PREFIX:-/usr}/include" "-DCMAKE_CXX_FLAGS=-DPVB_BACKEND_PATH=\\\"$2\\\"" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -llog" "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -llog" >/dev/null
  ninja -C "$1" -j"$JOBS" vulkan_pixel_bc
  llvm-strip --strip-unneeded -o "$3" "$1/libvulkan_pixel_bc.so"
  patchelf --remove-rpath "$3"
}
build_pbc "$P/build-module"   /vendor/lib64/hw/vulkan.r49.so "$DEPS/vulkan.pixelbc.so"
build_pbc "$P/build-portable" vulkan.r49.so                  "$DEPS/vulkan.pixelbc.portable.so"
echo "deps prontas em $DEPS"
