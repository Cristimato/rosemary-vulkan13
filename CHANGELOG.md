# Changelog

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
