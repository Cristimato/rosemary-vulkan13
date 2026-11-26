# rosemary-vulkan13

**Vulkan 1.3 na Mali-G76 do POCO M5s / Redmi Note 10S (MT6785, "rosemary"), sem trocar o kernel.**

> **Status: ✅ concluído (2026-11-25).** Driver Vulkan 1.3 estável na Mali-G76, validado com uma
> bateria de testes avançados (veja [Validação](#validação)), incluindo um port de PC com shaders
> bindless que não abria em nenhuma versão do driver original.

| | Antes (stock) | Depois (este módulo) |
|---|---|---|
| Driver Vulkan | Mali r32p1 | Mali **r49p1** (DDK r49p1-03bet0) |
| API Vulkan | 1.1.177 | **1.3.278** |
| Feature anunciada ao Android | `vulkan.version` 1.1 | `vulkan.version` **1.3** |
| Shaders bindless (`runtimeDescriptorArray`) | ❌ pipeline falha | ✅ convertidos pelo **vkshim** |
| GLES / interface do sistema | r32p1 | r32p1 (sem mudança) |
| Kernel | stock | stock (sem mudança) |

---

## Como funciona (resumo)

```
app/jogo ──► /system/lib64/libvulkan.so  (loader Android)
               │
               ▼
          hw/vulkan.mt6785.so   ← vkshim: reescreve SPIR-V bindless → arrays de tamanho fixo
               │
               ▼
          hw/vulkan.r49.so      ← HAL Vulkan r49p1 (donor MT6768, NEEDED renomeado)
               │
               ▼
          libGLES_ma49.so       ← blob Mali r49p1 com a checagem de GPU ID patchada
               │
               ▼
          /dev/mali0            ← kbase r32p1 do kernel stock (UK 11.38; o r49 é compatível)
```

Três peças:

1. **Blob r49p1 de outra GPU.** Não existe blob novo nativo para G76 (os aparelhos com G76 pararam
   no r3x). O blob do MT6768 (Mali-G52) tem o mesmo ISA Bifrost v7; só recusa a GPU numa checagem
   de ID. Um patch de 8 bytes remove essa checagem.
2. **Convivência com o r32p1.** O blob é renomeado para `libGLES_ma49.so` (SONAME + NEEDED, mesmo
   tamanho de string). O sistema continua usando o r32p1 para GLES/UI; só o Vulkan vai para o r49.
3. **vkshim.** A Mali-G76 não suporta `runtimeDescriptorArray`, nem no r32 nem no r49. O port usa
   shaders bindless e `vkCreateGraphicsPipelines` falhava com `VK_ERROR_INITIALIZATION_FAILED`. O
   vkshim intercepta `vkCreateShaderModule` e troca `OpTypeRuntimeArray` de imagem/sampler por
   `OpTypeArray` de tamanho fixo (8192 imagens / 256 samplers, compatíveis com os layouts usados pelos ports),
   removendo as capabilities e a extensão de descriptor indexing.

Detalhes, offsets e motivos: [docs/COMO-FUNCIONA.md](docs/COMO-FUNCIONA.md).

---

## Instalação

**Requisitos:** POCO M5s / Redmi Note 10S (MT6785) com MIUI 14 / Android 12 (vendor
`V14.0.17.0.TFFMIXM`), root KernelSU/SukiSU ou Magisk, kernel stock (kbase r32p1).

1. Baixe **`vk49_g76-v2.0-KSU.zip`** na [Release v2.0](../../releases/latest) (os "Source code" são gerados automaticamente pelo GitHub e não são o módulo) e instale pelo gerenciador de root.
2. Reinicie.
3. **KernelSU/SukiSU:** em cada jogo que vai usar o driver, abra *App Profile* e **desligue
   "Umount modules"**. Sem isso, o namespace do app desmonta o módulo e o jogo volta ao r32p1.
4. Confira (opcional, como root):
   ```sh
   su -c ./sysvk        # tools/probe/sysvk.c
   # GPU: Mali-G76 | API 1.3.278
   ```

**Desinstalar:** remova o módulo no gerenciador e reinicie (ou `rm -rf /data/adb/modules/vk49_g76`).

## Validação

Testes feitos no aparelho, com o kernel stock:

| Teste | Resultado |
|---|---|
| Instância e device pelo loader do sistema (`tools/probe/sysvk`) | ✅ Mali-G76, Vulkan 1.3.278, `vkCreateDevice` OK |
| Recursos do core 1.3 (`dynamicRendering`, `synchronization2`, `maintenance4`) | ✅ todos ativos |
| Ordem de carregamento (`/proc/self/maps`) | ✅ loader → vkshim → HAL r49 → `libGLES_ma49.so` |
| Compute shader: resultado conferido com a CPU e tempo comparado com o r32p1 | ✅ correto, desempenho igual |
| GLES r32p1 e Vulkan r49p1 no mesmo processo | ✅ sem conflito de símbolos |
| ABI kbase: r49 (UK 11.46) sobre o kernel r32p1 (UK 11.38) | ✅ só aviso de versão |
| Patch SPIR-V offline (`shim/spvtest`) em shaders bindless reais | ✅ módulos válidos, pipelines criados |
| Port de PC com descriptor indexing, testado de ponta a ponta | ✅ renderiza em Vulkan; antes os pipelines falhavam com `VK_ERROR_INITIALIZATION_FAILED` |
| HAL carregada de forma isolada (`tools/probe/shimvk`), debug ligado e desligado | ✅ |
| Persistência depois de reboot (overlay do KernelSU) | ✅ |
| Build reproduzível (`make_module.sh`) comparado com o módulo instalado | ✅ binários idênticos byte a byte |

## Diagnóstico

O vkshim não gera logs pesados por padrão. Para ligar o modo de diagnóstico:

```sh
su -c setprop debug.vkshim 1     # vale para apps abertos depois disso; some no reboot
```

Com o debug ligado, os pipelines que falham são despejados (SPIR-V + estado) em
`/data/user/0/<pacote>/cache/vkdump/`, e o logcat (`logcat -s vkshim`) mostra descriptor set
layouts, pools e alocações. Veja [docs/SOLUCAO-DE-PROBLEMAS.md](docs/SOLUCAO-DE-PROBLEMAS.md).

## Build a partir do código

```sh
# precisa: clang (Termux ou NDK), headers Vulkan, patchelf, python3, unzip, zip
tools/make_module.sh Mali-r49p1-MT6768-v0.1.0.zip
# -> out/vk49_g76-v2.zip
```

O zip donor (`Mali-r49p1-MT6768-v0.1.0.zip`, sha256 `0b5d439f…fd1c6a`) vem do projeto
[Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS](https://github.com/Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS).
O script confere o hash e os bytes originais antes de aplicar cada patch. O build é reproduzível:
os binários gerados são idênticos aos da release.

## Estrutura

```
module/            template do módulo (module.prop, customize.sh, xml de feature 1.3)
shim/              vkshim (HAL wrapper) + spvtest (testa o patch SPIR-V offline)
tools/
  make_module.sh   monta o zip instalável
  patch_blob.py    patches binários do blob r49p1 (com verificação)
  probe/           sysvk (via loader do sistema) e shimvk (carrega um shim direto)
docs/              como funciona, histórico, solução de problemas
```

## Histórico

O repositório começou como `rosemary-kernel-r49`, com o plano de compilar um kernel com kbase
r49p1. Os builds funcionaram, mas o boot travava no logo. Os testes mostraram depois que o kernel
novo **não era necessário**: o blob r49 funciona sobre o kbase r32p1 stock, e o motivo real da
falha do port era o bindless, não a versão do driver. O kit de kernel foi removido (fica no
histórico do git). Relato completo: [docs/HISTORICO.md](docs/HISTORICO.md).

## Créditos

- **Dracen-W**, pelo port r49p1 para MT6768 (blob donor + libgpud shim).
- **ARM**: os blobs Mali são proprietários e **não estão neste repositório**. Só a release os
  redistribui, como o módulo original do Dracen-W.
- vkshim, scripts e documentação: este projeto.
