# Changelog

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
- Recursos emulados e anunciados: `depthClamp`, `fillModeNonSolid`, `shaderClipDistance`,
  `shaderCullDistance`, `multiDrawIndirect` (`maxDrawIndirectCount` = 2^30).
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
