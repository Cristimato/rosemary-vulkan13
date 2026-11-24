# COMECE AQUI (sessão nova do Claude no Codespace)

Você é uma sessão nova. Leia este arquivo primeiro, depois o `HANDOFF.md` se precisar de detalhes.

## Onde paramos (2026-11-24)
- Projeto: kernel 4.19 do Redmi Note 10S (rosemary, MT6785) com o driver de GPU **Mali r49p1** + root **SukiSU**.
  Aqui só **compilamos**. Não faça fastboot/adb e não mexa em OC.
- **Build 1 já foi feito e funcionou** → release `r49-build1` (r49p1-04eac0 UK 11.46, KSU=y, LTO/CFI off).
- Problema achado no build 1: o SukiSU saiu com `KSU_VERSION 30007`, e o app SukiSU do celular exige **>= 32513**.
  O `build.sh` já foi corrigido e agora fixa `KSU_VERSION := 35008` (veja o bloco antes de "# 5) Toolchain").
- SukiSU **tem que ser a branch `builtin` (4bec532d)** + `patches/sukisu/0001-*.patch`. A `main` dá 86 erros no 4.19.

## Sua tarefa agora: gerar o build 2
1. `git pull`
2. Veja se o build antigo ainda existe: `ls ~/kbuild/src/out/arch/arm64/boot/Image.gz ~/kbuild/clang/bin/clang`
   - **Se existir**, faça o build incremental (poucos minutos):
     ```
     cd ~/kbuild/src
     sed -i 's/^KSU_VERSION[[:space:]]*:=.*/KSU_VERSION     := 35008/' KernelSU/kernel/Makefile
     export PATH="$HOME/kbuild/clang/bin:$PATH"
     make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- -j$(nproc) Image.gz 2>&1 | tee ~/kbuild/build2.log
     ```
   - **Se não existir** (o Codespace foi recriado), rode o completo: `./build.sh 2>&1 | tee ~/build2-full.log`.
     Ele já aplica tudo, incluindo a versão 35008.
3. Confira os resultados:
   - `grep -E "SukiSU.*version:" <log>` deve mostrar **35008**;
   - `zcat <Image.gz> | strings | grep -m1 "UK version"` deve mostrar r49p1;
   - `grep -E 'CONFIG_(KSU|MTK_GPU_VERSION|LTO_NONE)=' <.config>`.
4. Publique a release:
   ```
   gh release create r49-build2 <caminho>/Image.gz <caminho>/.config <log> --title "r49p1 build 2 (KSU 35008)" --notes "KSU_VERSION fixado em 35008"
   ```
5. Se der erro, salve as últimas 60 linhas em `errors.txt`, faça commit e push, e avise.
6. Ao terminar, diga ao usuário para **parar o Codespace** (Codespaces → ⋯ → Stop codespace) para economizar horas.
