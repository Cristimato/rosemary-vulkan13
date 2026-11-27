# Changelog

## v5.0 — 2026-11-27

Junta o trabalho das versões internas 4.8 a 4.9.4 (não publicadas separadamente). O foco é
compatibilidade: quase tudo que jogos e camadas de tradução (DXVK/VKD3D) pedem do Vulkan 1.3 está
nativo ou emulado. Desempenho fica para a v5.7.

### v4.9.4 — draw_indirect_count e depth_clip_control
- `VK_KHR_draw_indirect_count` / `drawIndirectCount`: N = maxDrawCount draws indiretos; o índice e o
  endereço do contador (buffer device address) vão por push constant e o VS (variante) joga para
  fora da tela os vértices dos draws com índice >= contador. O vkshim liga `bufferDeviceAddress` e
  `shaderInt64`, põe `SHADER_DEVICE_ADDRESS` nos buffers indiretos e `DEVICE_ADDRESS` nas alocações
  (só quando o app pede o recurso). Custo: o VS roda para os draws descartados.
- `VK_EXT_depth_clip_control`: `negativeOneToOne` vira z' = (z + w) / 2 no último estágio.
- Waterfall: número de voltas = tamanho do subgroup informado pelo driver (8 na G76).
- Não feito: `VK_EXT_color_write_enable`. O r49 não tem máscara de cor dinâmica (EDS3
  colorWriteMask = 0); o estado dinâmico exigiria pipelines sob demanda (até 256 variantes).
- c92 22 OK / 0 BUG / 1 limite; e47 23/23; 57 módulos válidos.

### v4.9.3 — descriptor indexing completo
Testado no aparelho antes de emular (probe `c92`, r49 puro): buffers com índice divergente já dão
certo; imagens/texel não (a G76 usa o índice de uma thread só); descritor nunca escrito derruba o
job inteiro; update-after-bind vale para imagens/texel e não para buffers; variable count é nativo;
arrays de 1.000.000 descritores são aceitos em todos os tipos.
- Índice NonUniform em imagens, storage images, texel buffers e runtime arrays: passe novo
  `spvemu_waterfall` (laço por `OpGroupNonUniformBroadcastFirst`, busca fora do `if`, que dentro
  dele o compilador troca `t[f]` por `t[i]`; em fragment shader ImplicitLod vira gradientes
  calculados antes do laço, bias vira escala 2^bias). Prop/conf `waterfall=0` desliga.
- Partially bound: bindings preenchidos com os fantasmas do nullDescriptor na alocação do set.
- Anunciados: `descriptorIndexing` e todos os campos (menos input attachment), extensão
  `VK_EXT_descriptor_indexing`; limites de update-after-bind = 1.000.000 (nível 3 do D3D12).
- Capabilities de dynamic indexing de texel buffer (5304/5305) também saem do SPIR-V.
- Limite: update-after-bind de UBO/SSBO usa o descritor do momento do bind (a Mali copia o
  descritor de buffer no bind). Imagens e texel buffers (o caso bindless) são reais.
- c92: shim 21 OK / 0 BUG / 1 limite; r49 puro 7 OK / 14 BUG-CRASH. e47 22/22. 55 módulos válidos.

### v4.9.2 — compatibilidade
Probe novo `tools/probe/c92` (cada teste em processo próprio; crash vira CRASH): r49 puro 0/5 nos
nulos (trava em todos), vkshim 10/10. e47 22/22.
- `VK_EXT_robustness2`: `nullDescriptor` por recursos fantasma zerados (buffer, texel buffer,
  imagens 1D/2D/array/cube/3D RGBA8, storage R32_UINT sem AFBC, sampler); `robustBufferAccess2`
  liga o `robustBufferAccess` do core. Também em templates, push descriptors e vertex buffers nulos.
  Limite: escrita em storage nulo vai para o fantasma (deveria ser descartada); view fantasma de
  imagem é 2D quando o tipo do shader não é conhecido.
- `VK_KHR_push_descriptor` (+ templates e as versões 2 do maintenance6): descriptor sets de pools
  por command buffer, zerados no `vkBeginCommandBuffer`.
- `VK_EXT_vertex_attribute_divisor`: apelido do KHR do driver.
- `VK_EXT_depth_clip_enable`: corte desligado = depthClamp exato; ligado = sem clamp (sem custo).
- `VK_EXT_memory_budget` (uso contado pelo vkshim + 80% da RAM livre), `VK_EXT_memory_priority` e
  `VK_EXT_pageable_device_local_memory` (aceitos e ignorados).
- `VK_EXT_multi_draw`: laço de draws com `gl_DrawID` = índice.
- Achado: o r49 trava (SIGSEGV) em `vkUpdateDescriptorSets` com qualquer handle nulo.

### v4.9 — medição e config por jogo
- HUD (`hud=1`): CSV por segundo em `Android/data/<pkg>/vkshim_hud.csv` + logcat a cada 5 s:
  fps, quadro (mediana/p95/pior), espera pela GPU, latência de submit, submits/draws por quadro,
  pipelines compilados, MHz/uso da GPU (ged) e temperatura. `vkshim-hud <pkg> [resumo]`.
- O timestamp do r49 na G76 não é contínuo entre submits: não serve para tempo ocupado da GPU
  (a espera da CPU é o indicador de limite de GPU).
- Config por jogo: `Android/data/<pkg>/vkshim.conf` (chave=valor; props `debug.vkshim.*` ganham).
  `vkshim-conf <pkg> set k=v`. Ferramentas em `/system/bin` do módulo.
- Módulo: `sepolicy.rule` + `service.sh` (`chmod 0444` no ged) para o HUD ler a GPU de dentro do app.

### v4.8 — correções
- depthClamp exato por pixel (`gl_FragDepth`); `debug.vkshim.clamp` 0/1/2. Corrige o chão
  transparente perto da câmera no BT3 (Dragon Rage). e47 `clamp_chao` lê a profundidade:
  v4.7 erro 0,77 em 100% dos pixels, v4.8 erro 0.
- PixelBC aprendido por jogo (`cache/vkshim_bc.txt`): 3 sessões sem textura BC = sem PixelBC.
- Log por pipeline no modo debug.

## v4.7 — 2026-11-26

**Recursos que deixam de ser só anunciados** (testados na GPU com `tools/probe/e47`: v4 9/19, v4.7 19/19)
- `shaderClipDistance`: recorte real. No último estágio antes da rasterização o `gl_ClipDistance`
  vira varying (location livre) e o fragment shader descarta o pixel quando alguma distância é
  negativa (`shim/spvemu.c`). Pipelines sem fragment shader e sem escrita de cor ganham um FS mínimo.
  `maxClipDistances`/`maxCullDistances` passam a informar 8 (o driver dizia 0).
- `depthClamp`: z de `gl_Position` preso a [0, w] em cada vértice (exato quando a primitiva não
  cruza near/far; numa que cruza, a profundidade é achatada por vértice).
- `multiDrawIndirect`: `gl_DrawID` correto. O índice vai num push constant extra do pipeline layout
  e é gravado antes de cada draw.
- `fillModeNonSolid`: LINE/POINT por um geometry shader gerado (triângulo -> arestas/vértices,
  varyings repassadas, Flat do vértice provocante, culling quando cull/front face/viewport são estáticos).

**Erratas do driver r49p1** (nomes da lista do MaliBridge; pesquisa, nenhum código copiado)
- MK-16 (Arm 3786517): com cull mode dinâmico pontos e linhas sumiam. **Confirmado no aparelho**;
  o vkshim manda `VK_CULL_MODE_NONE` enquanto um pipeline de pontos/linhas está ligado.
- MK-07 (Arm 4222073): views 2D/2D array de imagens 3D com `VK_REMAINING_*` ganham contagens explícitas.
- MK-11 (Arm 4021056): layout de stencil separado vira o layout combinado equivalente
  (`vkCreateRenderPass2` e renderização dinâmica).
- MK-06 (Arm 3786926): views usadas em `vkCmdBeginRendering` têm a destruição adiada (fila de 4096).
- Testadas e **não reproduzidas** neste driver (sem correção): MK-01 (espera interrompida por
  sinal) e MK-04 (estado dinâmico antes do bind: cull, depth, strides).

**Outros**
- Shaders inline (maintenance5, SPIR-V no `VkPipelineShaderStageCreateInfo`) passam pelos mesmos
  patches; antes escapavam do bindless/clipcull.
- Props novas: `debug.vkshim.clip/.clamp/.drawid/.nonsolid/.errata` (0 = comportamento da v4).
- Custo medido: ~0,09 µs a mais por `vkCmdBindPipeline`; draws inalterados. O PixelBC já custava
  ~0,25 µs por `vkCmdPushConstants` (histórico de estado dele); jogos sem texturas BC podem usar
  `debug.vkshim.bcn=0`.
- Testes: `tests/run.sh` valida as variantes do `spvemu` (28 módulos, `spirv-val`); probe `e47`.

**Ainda falta:** teste dentro de jogos reais com a v4.7.

## v4.0 — 2026-11-26

**Corrigido**
- Shader com `gl_ClipDistance`/`gl_CullDistance` derrubava o driver (SIGSEGV em
  `vkCreateGraphicsPipelines`). Novo passe `shim/clipcull.c` retira esses built-ins
  (formato do glslang e variáveis soltas). Os planos de recorte passam a ser ignorados.
- Shaders afetados por bugs do compilador Mali, como o erratum Arm 3843237 (bitwise vetorial,
  r18p0–r53p0) e o 3779215 (clamp com 0), além de constant folding de shift, spec-constant
  composite e loops de compute para wave 32. Usa os passes do fork leegao/SPIRV-Tools.
- Pipelines com `depthClampEnable` ou polígono LINE/POINT agora são saneados mesmo sem o
  recurso anunciado (antes derrubavam o driver ou falhavam).

**Novo**
- Recursos anunciados: `depthClamp`, `fillModeNonSolid`, `shaderClipDistance`,
  `shaderCullDistance`, `multiDrawIndirect` (`maxDrawIndirectCount` = 2^30). Não são nativos:
  clamp e polígono LINE/POINT são desligados, clip/cull são retirados e MDI vira N draws
  (`gl_DrawID` = 0). Veja a seção Limitações do README.
- Opcionais (`debug.vkshim.fake`): `multiViewport`, `vertexPipelineStoresAndAtomics`, descriptor
  indexing completo + `VK_EXT_descriptor_indexing`.
- Texturas BC1–BC5/BC7 decodificadas na GPU pelo PixelBC sem perda (`hw/vulkan.pixelbc.so`).
- Cache de pipelines persistente por app (`cache/vkshim_pcache.bin`) quando o app não usa um.
- Props `debug.vkshim.fake/.spv/.pcache/.bcn` para ligar/desligar cada parte.
- `tools/build_deps.sh` (dependências em commits fixos), `tests/run.sh`, probes `fullvk` e `v4test`.

**Notas**
- vkshim agora é C + C++ (SPIRV-Tools estático, libc++ estático): só depende de libc, libm,
  libdl e liblog, o que exige o namespace sphal.
- O perfil ETC2 do PixelBC foi testado e descartado: 32 diferenças de pixel em BC1/BC3 na G76.

## v3.0 — 2026-11-26
- Pacote portátil (driver ZIP importável). Revertido aqui e publicado no repositório próprio
  [mali-g76-vulkan13-portable](https://github.com/Cristimato/mali-g76-vulkan13-portable).

## v2.0 — 2026-11-25
- vkshim v2: intercepta só `vkCreateShaderModule` e `vkCreateGraphicsPipelines` no uso normal;
  sem cópia de SPIR-V nem logs por chamada.
- Modo diagnóstico opcional: `setprop debug.vkshim 1` (dumps em `cache/vkdump`, logs de descriptors).
- Módulo renomeado para "Vulkan 1.3 r49p1 + vkshim (Mali-G76)", versionCode 2.
- Build reproduzível: `tools/make_module.sh` + `tools/patch_blob.py`.
- Repositório reorganizado; kit de kernel r49p1 removido (abandonado, veja docs/HISTORICO.md).

## v1.0 — 2026-11-24
- Blob r49p1 (donor MT6768) com patch de GPU ID → Vulkan 1.3.278 na Mali-G76, kernel stock.
- vkshim: conversão de runtime descriptor arrays (bindless) para arrays fixos.
- Validado de ponta a ponta com um port de PC que usa shaders bindless.
