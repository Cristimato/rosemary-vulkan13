# HANDOFF — kernel rosemary com Mali r49p1

Você (Claude no Codespace) vai **só compilar** o kernel. Gravar no celular é feito depois, em outro lugar. **Não tente fazer fastboot ou adb daqui.**

## Contexto
- Aparelho: Redmi Note 10S "rosemary", MT6785 (Helio G95), GPU Mali-G76 MC4, root SukiSU (KernelSU), kernel 4.19 YuuriLegacy.
- Objetivo: trocar o driver kbase do kernel de **r32p1 para r49p1**. Assim o blob r49p1 de userspace (Vulkan 1.3) roda e os ports nativos de PC funcionam. Exemplo: o Naruto Storm 2 port hoje falha com `vkCreateGraphicsPipelines` = `VK_ERROR_INITIALIZATION_FAILED` e depois trava em `libGLES_mali.so` no r32p1.
- Base: `Pietro-Maltose/android_kernel_xiaomi_mt6785`, branch yuuri-main, commit `b95fedd83`.
- `patches/0001-*.patch` (um commit só, 603 arquivos) faz três coisas:
  - adiciona `drivers/gpu/mediatek/gpu_mali/mali_bifrost/mali-r49p1` (port do Dracen-W/android_kernel_xiaomi_mt6768_S, branch `Lineage-24.0-merlin-4.19-r49p1-features-stable`, com a plataforma mt6785 adaptada: include `pm_internal.h` e stub de `kbase_platform_dvfs_event`);
  - liga o SukiSU (`drivers/kernelsu`, `drivers/Makefile`, `drivers/Kconfig`);
  - corrige o `sortextable` no Termux. Essa correção é inofensiva em x86.
- O driver r49p1 já compilou limpo no celular (built-in.a). O build completo do kernel nunca terminou lá, porque o celular é lento demais.

## Tarefa
1. Rode `./build.sh`. Ele baixa a base, aplica o patch, clona o SukiSU no commit certo, baixa o Neutron Clang, configura e compila `Image.gz`.
2. Se der erro de compilação, corrija **no mínimo necessário** e registre cada correção como um novo `patches/0002-*.patch` (`git -C ~/kbuild/src diff > ...`). Tipos de erro esperados:
   - warnings tratados como erro pelo clang novo → `-Wno-...` só no Makefile do arquivo afetado;
   - símbolos do r49p1 que faltam na plataforma mt6785 → stub ou ajuste em `platform/mt6785`.
3. **Regras de config (não mudar):** KPROBES=y, KSU=y, KSU_MANUAL_HOOK=n, LTO e CFI desligados, MTK_GPU_VERSION="mali bifrost r49p1", MTK_GPU_MFG_COUNTER=n, LOCALVERSION="-r49".
4. Ao final, confira três coisas:
   - `strings out/vmlinux | grep "Linux version"`;
   - `grep -c mali-r49p1 ~/kbuild/build.log` > 0, ou `ls ~/kbuild/src/out/drivers/gpu/mediatek/gpu_mali/mali_bifrost/mali-r49p1`;
   - que **não** compilou o r32p1/r25p0.
5. Publique o resultado como release privada neste repositório:
   ```
   gh release create r49-build1 out/Image.gz out/.config out/build.log out/version.txt \
     --title "r49p1 build 1" --notes "Neutron clang, base b95fedd83 + 0001 (+0002 se houver)"
   ```
   e faça commit/push de qualquer `patches/0002-*.patch` novo.

## Avisos
- **Não** adicione susfs. O kernel em uso tem susfs v2.2.0, mas não há fonte compatível. Fica fora por enquanto.
- **Não** mexa em tabelas de frequência de CPU ou GPU (OC). Isso é feito por módulos separados no aparelho.
- O boot.img (header v2, kernel gzip + `kernel_dtb` separado) é remontado no celular com magiskboot. Daqui só sai o `Image.gz`.
- O SukiSU calcula a versão pelo número de commits. Por isso o clone dele precisa de histórico completo.
