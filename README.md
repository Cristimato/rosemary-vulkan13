# rosemary-vulkan13

**Vulkan 1.3 na Mali-G76 do POCO M5s / Redmi Note 10S (MT6785, "rosemary"), sem trocar o kernel.**

> **Versão atual: v4.7 (2026-11-26)** — recorte (ClipDistance), depthClamp, gl_DrawID e wireframe
> agora emulados de verdade, mais correções de erratas do driver. Veja o [CHANGELOG](CHANGELOG.md).
>
> **v4.0:** Blob r49p1 + **vkshim v4**: além dos shaders bindless,
> corrige bugs do compilador Mali, evita o crash de ClipDistance, anuncia recursos que faltam na
> G76 (vários sem o efeito real, veja [Limitações](#limitações)), decodifica texturas BC na GPU e
> guarda os pipelines em cache. Detalhes: [docs/PESQUISA-V4.md](docs/PESQUISA-V4.md). A v2.0
> continua disponível nas Releases.

| | Stock | v2.0 | v4.0 | **v4.7** |
|---|---|---|---|---|
| Driver / API Vulkan | r32p1 / 1.1.177 | r49p1 / 1.3.278 | r49p1 / 1.3.278 | r49p1 / **1.3.278** |
| Shaders bindless (`runtimeDescriptorArray`) | ❌ pipeline falha | ✅ convertidos | ✅ convertidos | ✅ convertidos |
| Shader com `gl_ClipDistance` | ❌ | ❌ **derruba o driver** | ⚠️ compila, recorte ignorado | ✅ **recorte real** (discard no FS) |
| Erratas do compilador Mali (Arm 3843237 e outros) | — | — | ✅ | ✅ |
| `depthClamp` | ❌ | ❌ | ⚠️ ignorado | ✅ z preso a [0,w] por vértice |
| `fillModeNonSolid` (wireframe) | ❌ | ❌ | ⚠️ vira FILL | ✅ geometry shader gerado |
| `multiDrawIndirect` | ❌ | ❌ | ⚠️ `gl_DrawID` = 0 | ✅ vários draws, `gl_DrawID` correto |
| Erratas do driver (MK-16 confirmada, MK-07/11/06) | — | — | ❌ | ✅ |
| Texturas BC1–BC7 | ❌ | ❌ | ✅ | ✅ (PixelBC, sem perda) |
| Cache de pipelines entre sessões | — | — | ✅ | ✅ (apps sem cache próprio) |
| GLES / interface / kernel | r32p1 / stock | sem mudança | sem mudança | sem mudança |

---

## Limitações

A Mali-G76 não tem esses recursos no hardware. Desde a v4.7 a maioria é **emulada** (o efeito
acontece), mas não é idêntica ao nativo em tudo:

| Recurso anunciado | v4.7 | Diferença para o nativo |
|---|---|---|
| `shaderClipDistance` | varying + discard no fragment shader | mesmos pixels para planos lineares; o discard tira o early-Z desses pipelines (`debug.vkshim.clip=0` volta a ignorar) |
| `shaderCullDistance` | **ignorado** | nenhuma primitiva é descartada (aparece geometria a mais, nunca a menos) |
| `depthClamp` | z preso a [0, w] em cada vértice | exato se a primitiva não cruza near/far; se cruza, a profundidade dos vértices de fora é achatada |
| `fillModeNonSolid` | geometry shader gerado | só pipelines com VS + FS e topologia de triângulos; sem culling se cull mode/front face/viewport forem dinâmicos; LINE/POINT custa um GS (lento no Mali, use só para depuração) |
| `multiDrawIndirect` | N draws, `gl_DrawID` por push constant | sem o slot de push constant (layout já usa 256 bytes) o `gl_DrawID` fica 0; mais custo de CPU com N alto |
| Descriptor indexing completo | **não emulado** (`debug.vkshim.fake=descidx`, desligado) | a conversão bindless para arrays fixos (8192 imagens / 256 samplers) cobre os layouts testados, não todos |
| `multiViewport`, `vertexPipelineStoresAndAtomics` | não emulados, desligados | anunciá-los faz o jogo escolher um caminho que quebra |

Cada emulação pode ser desligada (`debug.vkshim.clip/.clamp/.drawid/.nonsolid/.errata=0`), e
`su -c setprop debug.vkshim.fake none` faz o jogo ver a GPU real.

---|---|---|
| `shaderClipDistance` / `shaderCullDistance` | `gl_ClipDistance`/`gl_CullDistance` são retirados do shader | planos de recorte do usuário são ignorados: nada é recortado (água/espelhos/portais podem vazar) |
| `depthClamp` | o pipeline é criado com `depthClampEnable = VK_FALSE` (também no estado dinâmico) | geometria fora do intervalo de profundidade é cortada em vez de grampeada (buracos em sombras, objetos colados na câmera) |
| `fillModeNonSolid` | polígonos `LINE`/`POINT` viram `FILL` | wireframe e pontos saem como triângulos cheios |
| `multiDrawIndirect` | `vkCmdDraw(Indexed)Indirect` com N comandos vira N chamadas de 1 draw | resultado igual, exceto `gl_DrawID`, que fica sempre 0; mais custo de CPU com N alto |
| Descriptor indexing completo | **não emulado**; só aparece com `debug.vkshim.fake=descidx` (desligado por padrão). O que funciona é a conversão bindless para arrays fixos (8192 imagens / 256 samplers) | jogos com layouts maiores ou indexação não uniforme fora desse padrão podem falhar |
| `multiViewport`, `vertexPipelineStoresAndAtomics` | não emulados; opcionais, desligados por padrão | anunciá-los faz o jogo escolher um caminho que quebra |

Se um jogo renderizar errado por causa disso, desligue o anúncio com
`su -c setprop debug.vkshim.fake none` (ou só os que interessam, ex.: `mdi`) e reabra o jogo: ele
verá a GPU real e pode escolher outro caminho.

---

## Como funciona (resumo)

```
app/jogo ──► /system/lib64/libvulkan.so  (loader Android)
               │
               ▼
          hw/vulkan.mt6785.so   ← vkshim v4.7: bindless, erratas Mali/driver, recursos
               │                  emulados, cache de pipelines
               ▼
          hw/vulkan.pixelbc.so  ← PixelBC: texturas BC1–BC7 decodificadas na GPU
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

4. **vkshim v4.** Corrige shaders para bugs conhecidos do compilador Mali (passes do fork
   [leegao/SPIRV-Tools](https://github.com/leegao/SPIRV-Tools)), retira ClipDistance/CullDistance
   (que derrubavam o driver), anuncia depthClamp/fillModeNonSolid/multiDrawIndirect (com as
   [limitações](#limitações) acima), encadeia o
   [PixelBC](https://github.com/supplefrog/PixelBC) para texturas BC e mantém um cache de
   pipelines por app.

Detalhes, offsets e motivos: [docs/COMO-FUNCIONA.md](docs/COMO-FUNCIONA.md) e
[docs/PESQUISA-V4.md](docs/PESQUISA-V4.md).

---

## Instalação

**Requisitos:** POCO M5s / Redmi Note 10S (MT6785) com MIUI 14 / Android 12 (vendor
`V14.0.17.0.TFFMIXM`), root KernelSU/SukiSU ou Magisk, kernel stock (kbase r32p1).

1. Baixe **`vk49_g76-v4.7-KSU.zip`** na [Release mais recente](../../releases/latest) (os "Source code" são gerados automaticamente pelo GitHub e não são o módulo) e instale pelo gerenciador de root. Substitui a v4/v2 (mesmo id de módulo).
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

### v4.7

`tools/probe/e47` renderiza na GPU e confere pixels (driver puro / v4 / v4.7):

| Teste | r49 puro | v4 | v4.7 |
|---|---|---|---|
| Cull dinâmico com pontos/linhas (MK-16) | ❌ somem | ❌ | ✅ |
| Estado dinâmico antes do bind (MK-04) e espera com sinais (MK-01) | ✅ não reproduz | ✅ | ✅ |
| ClipDistance (bloco do glslang e variável solta) | — | ❌ nada cortado | ✅ exatamente metade |
| depthClamp com z além do far / antes do near | — | ❌ 0 pixels | ✅ 4096 pixels |
| `gl_DrawID` em `vkCmdDrawIndirect(drawCount=4)` | — | ❌ 0,0,0,0 | ✅ 0,1,2,3 |
| Wireframe / pontos / culling do wireframe | — | ❌ preenchido | ✅ |
| **Total** | | **9/19** | **19/19** |

Também: `v4test` (regressão da v4) ✅, `tests/run.sh` (28 módulos do `spvemu` válidos no `spirv-val`) ✅,
ASan/UBSan nos geradores ✅, 20 mil iterações sem vazamento ✅. **Falta teste em jogos reais.**

### v4.0

| Teste | Resultado |
|---|---|
| `tools/probe/v4test` na v2: pipeline com ClipDistance | ❌ SIGSEGV dentro do driver |
| `tools/probe/v4test` na v4: device com recursos anunciados, ClipDistance (1 e 2 planos), depthClamp + LINE, compute, `vkCmdDrawIndirect(drawCount=4)` executado na GPU | ✅ |
| Perfis `debug.vkshim.fake=all` e `=none` | ✅ |
| Cache de pipelines (grava, recarrega, valida cabeçalho) | ✅ |
| Passes Mali em 16 shaders reais de um port: 14 alterados, 0 erros, ~0,85 ms/shader | ✅ válidos (`spirv-val`) |
| PixelBC sem perda na G76: probe BC1–BC5 / probe BC7 | ✅ 535/0 · 20046/0 |
| Cadeia vkshim → PixelBC → r49 | ✅ |
| Build reproduzível (`make_module.sh`) | ✅ idêntico ao pacote testado |

Ainda falta: teste dentro de jogos reais com a v4.

### v2.0

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

## Ajustes

Cada parte pode ser desligada sem reinstalar; vale para apps abertos depois:

| Prop | Padrão | Efeito |
|---|---|---|
| `debug.vkshim.fake` | `depthclamp,nonsolid,clipdist,mdi` | recursos anunciados (também `multivp`, `vsstores`, `descidx`, `all`, `none`) |
| `debug.vkshim.clip` | `1` | `0` = planos de recorte ignorados (v4) |
| `debug.vkshim.clamp` | `1` | `0` = depthClamp ignorado (v4) |
| `debug.vkshim.drawid` | `1` | `0` = `gl_DrawID` fica 0 (v4) |
| `debug.vkshim.nonsolid` | `1` | `0` = LINE/POINT viram FILL (v4) |
| `debug.vkshim.errata` | `1` | `0` = sem as correções MK-16/07/11/06 |
| `debug.vkshim.spv` | `1` | `0` desliga os passes Mali de SPIR-V |
| `debug.vkshim.pcache` | `1` | `0` desliga o cache de pipelines |
| `debug.vkshim.bcn` | `1` | `0` tira o PixelBC (texturas BC) |

`vsstores` e `descidx` ficam desligados por padrão porque não são emulados de verdade: anunciados,
fazem o jogo escolher um caminho que depois falha.

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
# precisa: clang/clang++, cmake, ninja, git, glslangValidator, headers Vulkan, patchelf, python3, zip
tools/build_deps.sh                                  # SPIRV-Tools (fork) + PixelBC, commits fixos
tools/make_module.sh Mali-r49p1-MT6768-v0.1.0.zip    # -> out/vk49_g76-v4.7-KSU.zip
tests/run.sh                                         # testes offline dos passes SPIR-V
```

O zip donor (`Mali-r49p1-MT6768-v0.1.0.zip`, sha256 `0b5d439f…fd1c6a`) vem do projeto
[Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS](https://github.com/Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS).
O script confere o hash e os bytes originais antes de aplicar cada patch. O build é reproduzível:
os binários gerados são idênticos aos da release.

## Estrutura

```
module/            template do módulo (module.prop, customize.sh, xml de feature 1.3)
shim/              vkshim v4.7: shim.c, spvemu.c (emulações), clipcull.c, spvfix.cpp (passes Mali)
patches/           patch do PixelBC (caminho do backend)
tests/             shaders de teste + run.sh (validação offline)
tools/
  build_deps.sh    baixa e compila SPIRV-Tools (fork leegao) e PixelBC em commits fixos
  make_module.sh   monta o zip instalável
  patch_blob.py    patches binários do blob r49p1 (com verificação)
  probe/           sysvk, shimvk, fullvk, v4test, e47 (erratas + emulações na GPU, benchmark)
docs/              como funciona, pesquisa da v4, histórico, solução de problemas
THIRD_PARTY.md     componentes de terceiros e licenças
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
- **leegao**, pelo fork do SPIRV-Tools com os passes para o compilador Mali e pela pesquisa do
  bionic-vulkan-wrapper.
- **supplefrog**, pelo PixelBC (MIT).
- **MaliBridge** (ASEbrahim e colaboradores, GPL-3): a lista de erratas do driver (MK-xx) e a ideia do clip por varying + discard serviram de pesquisa; nenhum código foi copiado.
- vkshim, `clipcull.c`, scripts e documentação: este projeto. Veja [THIRD_PARTY.md](THIRD_PARTY.md).
