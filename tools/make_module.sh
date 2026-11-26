#!/bin/sh
# Gera o zip instalável (KernelSU/Magisk) do módulo vk49_g76 (vkshim v4.7).
# Uso: tools/make_module.sh <Mali-r49p1-MT6768-v0.1.0.zip>
# Saída: out/vk49_g76-<versão>-KSU.zip
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DONOR=${1:?uso: $0 <Mali-r49p1-MT6768-v0.1.0.zip>}
DONOR_SHA=0b5d439f756dcc4236063a3186d2f158ad7ff8333b9d3ceee4cc3b3154fd1c6a
echo "$DONOR_SHA  $DONOR" | sha256sum -c - >/dev/null || { echo "sha256 do zip donor não confere"; exit 2; }

# 0) dependências (SPIRV-Tools do fork + PixelBC), em commits fixos
[ -f "$ROOT/deps/vulkan.pixelbc.so" ] || sh "$ROOT/tools/build_deps.sh"

OUT=$ROOT/out
WORK=$OUT/work
rm -rf "$WORK"; mkdir -p "$WORK/donor"
unzip -q "$DONOR" -d "$WORK/donor"

# 1) estrutura do módulo (template versionado)
cp -r "$ROOT/module" "$WORK/mod"
LIB=$WORK/mod/system/vendor/lib64

# 2) blob r49p1 patchado (GPU-check + renomes)
python3 -I "$ROOT/tools/patch_blob.py" "$WORK/donor" "$LIB"

# 3) vkshim v4.7 como HAL (vulkan.mt6785.so é o nome que o loader procura) + PixelBC
sh "$ROOT/shim/build.sh" "$LIB/hw/vulkan.mt6785.so" module
cp "$ROOT/deps/vulkan.pixelbc.so" "$LIB/hw/vulkan.pixelbc.so"
cp "$ROOT/THIRD_PARTY.md" "$WORK/mod/"

VER=$(sed -n 's/^version=//p' "$WORK/mod/module.prop")
ZIP=$OUT/vk49_g76-$VER-KSU.zip
rm -f "$ZIP"
(cd "$WORK/mod" && find . -exec touch -d @1795132800 {} + && zip -q -X -r -9 "$ZIP" .)
rm -rf "$WORK"
echo "pronto: $ZIP"
