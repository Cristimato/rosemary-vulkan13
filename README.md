# rosemary-vulkan13

**Vulkan 1.3 na Mali-G76 do POCO M5s / Redmi Note 10S (MT6785, "rosemary"), sem trocar o kernel.**

> **Versão atual: v5.7 (2026-11-27)** — primeira versão de desempenho, tudo por jogo no
> `vkshim.conf`: limitador de FPS com ritmo estável, samplers mais baratos (no benchmark, `aniso=4`
> deixa a carga de textura 8,4x mais rápida e `mip=near` 2,1x com qualidade quase igual), FP16
> experimental e o perfil turbo do Game Mode. Medições reais, inclusive o que não ajudou, no
> [CHANGELOG](CHANGELOG.md).
>
> **v5.0:** compatibilidade quase completa do Vulkan 1.3 (robustness2, push descriptors,
> descriptor indexing completo, `draw_indirect_count`, depthClamp exato, `depth_clip_enable`/
> `_control`, multi draw, orçamento de memória), HUD e config por jogo.

| | Stock | v2.0 | v4.7 | **v5.0 / v5.7** |
|---|---|---|---|---|
| Driver / API Vulkan | r32p1 / 1.1.177 | r49p1 / 1.3.278 | r49p1 / 1.3.278 | r49p1 / **1.3.278** |
| Shaders bindless (`runtimeDescriptorArray`) | ❌ pipeline falha | ✅ convertidos | ✅ convertidos | ✅ convertidos |
| Descriptor indexing (não uniforme, partially bound, update-after-bind) | ❌ | ❌ | ❌ | ✅ (UAB de buffer: valor do bind) |
| `VK_EXT_robustness2` (nullDescriptor; o DXVK exige) | ❌ | ❌ | ❌ | ✅ |
| `VK_KHR_push_descriptor` | ❌ | ❌ | ❌ | ✅ |
| `drawIndirectCount` | ❌ | ❌ | ❌ | ✅ |
| `gl_ClipDistance` | ❌ | ❌ derruba o driver | ✅ recorte real | ✅ recorte real |
| `depthClamp` | ❌ | ❌ | ⚠️ por vértice | ✅ **exato por pixel** |
| `depth_clip_enable` / `depth_clip_control` | ❌ | ❌ | ❌ | ✅ |
| `fillModeNonSolid` / `multiDrawIndirect` / `multi_draw` | ❌ | ❌ | ✅ / ✅ / ❌ | ✅ / ✅ / ✅ |
| `memory_budget`, `memory_priority`, divisor EXT | ❌ | ❌ | ❌ | ✅ |
| Erratas do compilador e do driver Mali | — | — | ✅ | ✅ |
| Texturas BC1–BC7 (PixelBC) | ❌ | ❌ | ✅ | ✅ (sai sozinho em jogo sem BC) |
| HUD de medição e config por jogo | — | — | — | ✅ |
| Limitador de FPS, samplers por jogo, FP16 (v5.7) | — | — | — | ✅ (desligados por padrão) |
| GLES / interface / kernel | r32p1 / stock | sem mudança | sem mudança | sem mudança |

---

## Limitações

A Mali-G76 não tem esses recursos no hardware. Na v5.0 eles são **emulados** (o efeito acontece),
com estas diferenças para o nativo:

| Recurso | Como | Diferença para o nativo |
|---|---|---|
| `shaderClipDistance` | varying + discard no fragment shader | mesmos pixels; o discard tira o early-Z desses pipelines |
| `shaderCullDistance` | **ignorado** | aparece geometria a mais, nunca a menos |
| `depthClamp` | `gl_FragDepth` por pixel | exato; perde o early-Z nesses pipelines. Com depth bias usa o modo por vértice |
| Índice não uniforme de imagens | laço waterfall no shader | exato; custa até 8 voltas quando o índice diverge no warp |
| Update-after-bind de UBO/SSBO | — | vale o descritor do momento do bind (imagens e texel buffers são reais) |
| `nullDescriptor` | recursos fantasma zerados | escrita em storage nulo vai para o fantasma (deveria sumir); view fantasma é 2D |
| `drawIndirectCount` | N draws + teste no VS | o VS roda para os draws descartados |
| `fillModeNonSolid` | geometry shader gerado | só VS + FS com triângulos; GS é lento no Mali |
| `multiViewport`, `vertexPipelineStoresAndAtomics`, `color_write_enable` | não emulados | não anunciados |

Cada emulação pode ser desligada (veja [Ajustes](#ajustes)).

---

## Como funciona (resumo)

```
app/jogo ──► /system/lib64/libvulkan.so  (loader Android)
               │
               ▼
          hw/vulkan.mt6785.so   ← vkshim v5.7: bindless, erratas Mali/driver, recursos
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

1. Baixe **`vk49_g76-v5.7-KSU.zip`** na [Release mais recente](../../releases/latest) (os "Source code" são gerados automaticamente pelo GitHub e não são o módulo) e instale pelo gerenciador de root. Substitui a v5.0/v4.x/v2 (mesmo id de módulo).
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

### v5.7

`tools/probe/p57` (benchmark de GPU, 1280x720, mediana de 40 quadros; `p57 cmp` mede PSNR):
o vkshim não custa nada sobre o r49 puro; na carga de textura com aniso 16x, `aniso=4` = 8,4x mais
rápido, `mip=near` = 2,1x (37 dB). Tabela completa e o teste no BT3 no [CHANGELOG](CHANGELOG.md).
c92 22 OK / 1 limite e e47 23/23 seguem iguais à v5.0.

### v5.0

`tools/probe/c92` (cada teste em processo próprio; crash do driver vira CRASH):

| | r49 puro | **v5.0** |
|---|---|---|
| nullDescriptor (SSBO, UBO, imagem, texel, storage image) | 5 CRASH | 5 OK |
| push descriptor (direto e template) | sem a extensão | 2 OK |
| multi_draw, draw_indirect_count, memory_budget, divisor EXT | sem as extensões | 4 OK |
| descriptor indexing (não uniforme em 6 tipos, partially bound, UAB, arrays de 1.000.000) | 1 OK, resto falha | 10 OK + 1 limite (UAB de buffer) |

`tools/probe/e47`: 23/23 (inclui `clamp_chao`, que lê a profundidade: v4.7 erro 0,77 em todos os
pixels do chão, v5.0 erro 0; `depthclip_*` e `depthclip_control_gl`). `tests/run.sh`: 57 módulos
gerados, todos válidos no `spirv-val`. Testado no BT3 (Dragon Rage): o chão transparente da v4.7
sumiu.

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

**Por jogo (v5.0):** `Android/data/<pacote>/vkshim.conf`, uma chave por linha (`chave=valor`), lido
quando o jogo abre. Dá para editar sem root ou usar o comando do módulo:

```sh
su -c vkshim-conf com.exemplo.jogo set hud=1 clamp=1
su -c vkshim-conf com.exemplo.jogo show
```

**Global:** a prop `debug.vkshim.<chave>` tem a mesma chave e ganha do arquivo.

| Chave | Padrão | Efeito |
|---|---|---|
| `fake` | `depthclamp,nonsolid,clipdist,mdi` | recursos anunciados (também `all`, `none`) |
| `clamp` | `2` | `2` exato por pixel; `1` por vértice (v4.7); `0` ignorado |
| `clip` / `drawid` / `nonsolid` | `1` | `0` desliga a emulação |
| `waterfall` | `1` | `0` = índice não uniforme de imagem sem o laço |
| `errata` | `1` | `0` = sem as correções MK-16/07/11/06 |
| `spv` / `pcache` | `1` | `0` desliga os passes Mali / o cache de pipelines |
| `bcn` | automático | `1`/`0` força o PixelBC (sem a chave: sai depois de 3 sessões sem textura BC) |
| `hud` | `0` | `1` grava `Android/data/<pacote>/vkshim_hud.csv` (fps, quadro, espera pela GPU, draws, MHz, °C) |
| `fps` | `0` | v5.7: limita a N quadros por segundo com intervalo constante (ex.: `30`) |
| `aniso` | do app | v5.7: teto da filtragem anisotrópica (`4`, `2`; `0` desliga). O mais caro na Mali |
| `mip` | do app | v5.7: `near` troca trilinear por bilinear (quase igual na imagem) |
| `lodbias` | `0` | v5.7: soma ao LOD (`0.5` = texturas um pouco mais borradas e mais leves) |
| `fp16` | `0` | v5.7, experimental: FP16 nos fragment shaders (ganho pequeno medido; teste por jogo) |
| `turbo` | `0` | lido pelo Game Mode do rosemary (piso de CPU no máximo), não pelo vkshim |

**Qual ajuste usar:** veja no HUD se o jogo espera pela GPU (`espera_ms` alta, `gpu_uso_pct` perto
de 100%). Se espera, `aniso`/`mip`/`lodbias` ajudam. Se não espera e o FPS é baixo, o limite é a
CPU: nesse caso só o turbo ajuda (no BT3 em 1x: +11%), e o `fps=30` deixa o ritmo estável.

`su -c vkshim-hud <pacote>` mostra o HUD ao vivo; `vkshim-hud <pacote> resumo` dá as médias.

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
tools/make_module.sh Mali-r49p1-MT6768-v0.1.0.zip    # -> out/vk49_g76-v5.7-KSU.zip
tests/run.sh                                         # testes offline dos passes SPIR-V
```

O zip donor (`Mali-r49p1-MT6768-v0.1.0.zip`, sha256 `0b5d439f…fd1c6a`) vem do projeto
[Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS](https://github.com/Dracen-W/r49p1_GPUDriverUpgrade_for_redmi_merlinx_mt6768_vendorS).
O script confere o hash e os bytes originais antes de aplicar cada patch. O build é reproduzível:
os binários gerados são idênticos aos da release.

## Estrutura

```
module/            template do módulo (module.prop, customize.sh, service.sh, sepolicy.rule, bin/)
shim/              vkshim v5.7: shim.c, spvemu.c (emulações + waterfall), compat.c (extensões),
                   hud.c (medição), clipcull.c, spvfix.cpp (passes Mali)
patches/           patch do PixelBC (caminho do backend)
tests/             shaders de teste + run.sh (validação offline)
tools/
  build_deps.sh    baixa e compila SPIRV-Tools (fork leegao) e PixelBC em commits fixos
  make_module.sh   monta o zip instalável
  patch_blob.py    patches binários do blob r49p1 (com verificação)
  probe/           sysvk, shimvk, fullvk, v4test, e47 (render), c92 (compatibilidade), p57 (benchmark)
  vkshim-conf/-hud config por jogo e HUD (também em module/system/bin)
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
