# Changelog

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
- Naruto Storm 2 (`com.naruto.storm2`) funcionando, ~28 fps em 960x540.
