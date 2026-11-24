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
git -C src clean -qfdx

# 3) Patch r49p1 + SukiSU hookup
git -C src apply --whitespace=nowarn "$HERE"/patches/0001-*.patch

# 4) SukiSU: o patch cria o symlink KernelSU -> ../vk/sukisu. Substitui por clone real.
#    Precisa de histórico COMPLETO (kver = 30000 + nº de commits); não usar --depth.
rm -f src/KernelSU
git clone -q https://github.com/SukiSU-Ultra/SukiSU-Ultra src/KernelSU
git -C src/KernelSU checkout -q 1439545f12bffe0263690ba862139b70ee5f20cf

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
  -d MTK_GPU_MFG_COUNTER \
  --set-str MTK_GPU_VERSION "mali bifrost r49p1" \
  --set-str LOCALVERSION "-r49" -d LOCALVERSION_AUTO
make "${MK[@]}" olddefconfig

# Conferência
grep -E '^CONFIG_(KPROBES|KSU|KSU_MANUAL_HOOK|LTO_NONE|CFI_CLANG|MTK_GPU_VERSION|LOCALVERSION)=' out/.config || true
grep -q 'CONFIG_MTK_GPU_VERSION="mali bifrost r49p1"' out/.config || { echo "ERRO: GPU version não aplicou"; exit 1; }
grep -q '^CONFIG_KSU=y' out/.config || { echo "ERRO: KSU não ficou =y (KPROBES?)"; exit 1; }

# 7) Build
make "${MK[@]}" -j"$(nproc)" Image.gz 2>&1 | tee "$WORK/build.log"

# 8) Saída
mkdir -p "$HERE/out"
cp out/arch/arm64/boot/Image.gz out/.config "$HERE/out/"
cp "$WORK/build.log" "$HERE/out/"
strings out/vmlinux | grep -m1 "Linux version" | tee "$HERE/out/version.txt"
ls -l "$HERE/out"
echo "OK: $HERE/out/Image.gz"
