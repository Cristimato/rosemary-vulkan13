#!/bin/sh
# Gera o zip instalavel (KernelSU/Magisk) do modulo vk49_g76.
# Uso: tools/make_module.sh <Mali-r49p1-MT6768-v0.1.0.zip>
# Saida: out/vk49_g76-v<versao>.zip
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DONOR=${1:?uso: $0 <Mali-r49p1-MT6768-v0.1.0.zip>}
DONOR_SHA=0b5d439f756dcc4236063a3186d2f158ad7ff8333b9d3ceee4cc3b3154fd1c6a

echo "$DONOR_SHA  $DONOR" | sha256sum -c - >/dev/null || { echo "sha256 do zip donor nao confere"; exit 2; }

OUT=$ROOT/out
WORK=$OUT/work
rm -rf "$WORK"; mkdir -p "$WORK/donor"
unzip -q "$DONOR" -d "$WORK/donor"

# 1) estrutura do modulo (template versionado)
cp -r "$ROOT/module" "$WORK/mod"
LIB=$WORK/mod/system/vendor/lib64

# 2) blob r49p1 patchado (GPU-check + renomes)
python3 -I "$ROOT/tools/patch_blob.py" "$WORK/donor" "$LIB"

# 3) vkshim como HAL (vulkan.mt6785.so e o nome que o loader procura)
sh "$ROOT/shim/build.sh" "$LIB/hw/vulkan.mt6785.so"

VER=$(sed -n 's/^version=//p' "$WORK/mod/module.prop")
ZIP=$OUT/vk49_g76-$VER.zip
rm -f "$ZIP"
(cd "$WORK/mod" && zip -q -r -9 "$ZIP" .)
rm -rf "$WORK"
echo "pronto: $ZIP"
