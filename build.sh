#!/usr/bin/env bash
# Build kernel rosemary (MT6785) com driver Mali r49p1 + SukiSU.
# Uso: ./build.sh            (no Codespace / Ubuntu x86_64)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/kbuild}"
mkdir -p "$WORK"; cd "$WORK"

# 1) Dependências
if command -v apt-get >/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y -qq bc bison flex libssl-dev libelf-dev cpio lz4 \
       zstd python3 git make curl wget rsync xz-utils >/dev/null
fi

# 2) Kernel base: Pietro-Maltose yuuri-main @ b95fedd83
BASE=b95fedd83b20ab2f96d698a06f765eba774d849d
if [ ! -d src/.git ]; then
  git init -q src
  git -C src remote add origin https://github.com/Pietro-Maltose/android_kernel_xiaomi_mt6785.git
fi
git -C src fetch -q --depth=1 origin "$BASE"
git -C src checkout -q -f FETCH_HEAD
git -C src clean -qffdx   # -ff: remove também o clone aninhado src/KernelSU de execuções anteriores

# 3) Patches: 0001 (r49p1 + SukiSU hookup) e correções 0002+ em ordem
for p in "$HERE"/patches/[0-9][0-9][0-9][0-9]-*.patch; do
  git -C src apply --whitespace=nowarn "$p"
done

# 4) SukiSU: o patch cria o symlink KernelSU -> ../vk/sukisu. Substitui por clone real.
#    Precisa de histórico COMPLETO (kver = 30000 + nº de commits); não usar --depth.
rm -f src/KernelSU
git clone -q https://github.com/SukiSU-Ultra/SukiSU-Ultra src/KernelSU
# Branch builtin = linha non-GKI (4.19). A main (1439545f) é só GKI/kernels novos.
git -C src/KernelSU checkout -q 4bec532d     # origin/builtin
# Correções mínimas de compat 4.19 no SukiSU (static_call.h, access_ok de 3 args)
for p in "$HERE"/patches/sukisu/[0-9][0-9][0-9][0-9]-*.patch; do
  git -C src/KernelSU apply --whitespace=nowarn "$p"
done

# Versão do KernelSU: a builtin calcula pela contagem de commits da main (~30007), mas o
# manager instalado exige >= 32513 e o kernel atual é 35008. Fixa em 35008.
sed -i 's/^KSU_VERSION[[:space:]]*:=.*/KSU_VERSION     := 35008/' src/KernelSU/kernel/Makefile
grep -q '^KSU_VERSION     := 35008' src/KernelSU/kernel/Makefile || { echo "ERRO: KSU_VERSION nao fixou"; exit 1; }

# 5) Toolchain: Neutron Clang (o kernel original foi feito com Neutron clang 23)
TC="$WORK/clang"
if [ ! -x "$TC/bin/clang" ]; then
  mkdir -p "$TC"; cd "$TC"
  curl -sLo antman https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman
  chmod +x antman && ./antman -S && ./antman --patch=glibc || true
  cd "$WORK"
fi
export PATH="$TC/bin:$PATH"
clang --version | head -1

# 6) Config
cd src
MK=(O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang
    CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-)
make "${MK[@]}" rosemary_defconfig
scripts/config --file out/.config \
  -e KPROBES -e KPROBE_EVENTS -e KSU -d KSU_MANUAL_HOOK \
  -d LTO_CLANG -d LTO -e LTO_NONE -d CFI_CLANG \
  -d MTK_GPU_MFG_COUNTER -d KPM -d KSU_SUSFS -d KSU_FEATURE_ADBROOT \
  --set-str MTK_GPU_VERSION "mali bifrost r49p1" \
  --set-str LOCALVERSION "-r49" -d LOCALVERSION_AUTO
make "${MK[@]}" olddefconfig

# Conferência
grep -E '^CONFIG_(KPROBES|KSU|KSU_MANUAL_HOOK|LTO_NONE|CFI_CLANG|MTK_GPU_VERSION|LOCALVERSION)=' out/.config || true
grep -q 'CONFIG_MTK_GPU_VERSION="mali bifrost r49p1"' out/.config || { echo "ERRO: GPU version não aplicou"; exit 1; }
grep -q '^CONFIG_KSU=y' out/.config || { echo "ERRO: KSU não ficou =y (KPROBES?)"; exit 1; }

# 7) Build
make "${MK[@]}" -j"$(nproc)" Image.gz Image.gz-dtb dtbs 2>&1 | tee "$WORK/build.log"

# 8) Saída
mkdir -p "$HERE/out"
cp out/arch/arm64/boot/Image.gz out/.config "$HERE/out/"
# dtbs da MESMA árvore (build2 usou o dtb do kernel antigo -> bootloop na logo)
cp out/arch/arm64/boot/Image.gz-dtb "$HERE/out/" 2>/dev/null || true
find out/arch/arm64/boot/dts/mediatek -maxdepth 1 \( -name "mt6785*.dtb" -o -name "rosemary*.dtb*" \) -exec cp {} "$HERE/out/" \;
[ -n "$(ls "$HERE"/out/mt6785*.dtb 2>/dev/null)" ] || { echo "ERRO: mt6785.dtb não gerado"; exit 1; }
cp "$WORK/build.log" "$HERE/out/"
# (strings em arquivo antes do grep -m1: com pipefail, o SIGPIPE do strings abortava o script)
strings out/vmlinux > "$WORK/vmlinux.strings"
grep -m1 "Linux version" "$WORK/vmlinux.strings" | tee "$HERE/out/version.txt"
ls -l "$HERE/out"
echo "OK: $HERE/out/Image.gz"
