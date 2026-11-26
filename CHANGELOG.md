# Changelog

## v3.0 — 2026-11-26 — pacote portátil
- Novo **`vk49_g76-v3.0-portatil.zip`**: driver ZIP importável (meta.json na raiz) para launchers
  estilo adrenotools. Traz vkshim + HAL r49 + blob; não precisa de módulo nem root.
- vkshim: build `portable` (`shim/build.sh <saida> portable`) procura `vulkan.r49.so` na pasta do
  pacote, ao lado do próprio `.so` e, por último, em `/vendor/lib64/hw`.
- `meta.json` com `backend: panvk` / `panvkFrontend: bifrost-jm` / `gpuModels: [G76]` para passar
  no filtro de família desses launchers (antes o pacote era recusado como família "Unknown").
- `tools/make_portable.sh`: build reproduzível do pacote. O módulo KSU (v2.0) não mudou
  (vkshim do módulo continua byte a byte igual).

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
