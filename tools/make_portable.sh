#!/bin/sh
# Gera o pacote PORTATIL do driver (zip com meta.json na raiz) para launchers
# que importam driver Vulkan proprio (estilo adrenotools / "Import driver ZIP"),
# NAO precisa do modulo KSU nem de root.
# Uso: tools/make_portable.sh <Mali-r49p1-MT6768-v0.1.0.zip>
# Saida: out/vk49_g76-v<versao>-portatil.zip
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DONOR=${1:?uso: $0 <Mali-r49p1-MT6768-v0.1.0.zip>}
DONOR_SHA=0b5d439f756dcc4236063a3186d2f158ad7ff8333b9d3ceee4cc3b3154fd1c6a
VER=3.0

echo "$DONOR_SHA  $DONOR" | sha256sum -c - >/dev/null || { echo "sha256 do zip donor nao confere"; exit 2; }

OUT=$ROOT/out
WORK=$OUT/work_portable
rm -rf "$WORK"; mkdir -p "$WORK/donor" "$WORK/lib" "$WORK/pkg"
unzip -q "$DONOR" -d "$WORK/donor"

# 1) blob r49p1 patchado (mesmo do modulo)
python3 -I "$ROOT/tools/patch_blob.py" "$WORK/donor" "$WORK/lib"

# 2) tudo numa pasta so (o launcher usa a pasta do pacote como search path)
find "$WORK/lib" -type f -name '*.so' -exec cp {} "$WORK/pkg/" \;

# 3) vkshim portatil: procura vulkan.r49.so ao lado dele
sh "$ROOT/shim/build.sh" "$WORK/pkg/vulkan.mali_r49.so" portable

# 4) meta.json. "backend": "panvk" + "panvkFrontend": "bifrost-jm" so existem
#    para passar no filtro de familia do launcher (ele so conhece
#    Turnip/PanVK/RADV/PowerVR pelo texto); o driver e o blob ARM, nao Mesa.
cat > "$WORK/pkg/meta.json" <<EOF
{
  "schemaVersion": 1,
  "name": "Mali-G76 r49p1 Vulkan 1.3 (vkshim)",
  "description": "Blob ARM r49p1 + vkshim, portatil (nao precisa do modulo vk49_g76). Mali-G76/Bifrost. Tag panvk so para o filtro do launcher.",
  "author": "Cristimato",
  "packageVersion": "$VER",
  "vendor": "ARM",
  "driverVersion": "r49p1 (Vulkan 1.3.278)",
  "minApi": 28,
  "backend": "panvk",
  "panvkFrontend": "bifrost-jm",
  "gpuModels": ["G76"],
  "libraryName": "vulkan.mali_r49.so"
}
EOF

ZIP=$OUT/vk49_g76-v$VER-portatil.zip
rm -f "$ZIP"
chmod 644 "$WORK/pkg/"*
(cd "$WORK/pkg" && zip -q -X -9 "$ZIP" meta.json *.so)
rm -rf "$WORK"
echo "pronto: $ZIP"
