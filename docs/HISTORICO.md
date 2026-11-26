# Histórico do projeto

## Objetivo

Melhorar o desempenho e ter Vulkan 1.3 na Mali-G76 para a nova leva de ports de jogos de PC para
Android. O port usado como referência não abria no driver stock.

## Fase 1: kernel r49p1 (abandonada)

**Hipótese:** o blob r49 precisava de um kbase r49 no kernel, como no port de referência do
Dracen-W para MT6768.

- Fonte: YuuriLegacy (`yuuri-main` b95fedd, cip137) + kbase r49p1 (do Dracen-W) adaptado ao
  `platform/mt6785` + SukiSU (branch `builtin`, `KSU_VERSION` fixado em 35008).
- Builds feitos no GitHub Codespaces (releases `r49-build1..3`, já removidas):
  - build1: compilou (Neutron clang 24, r49p1 UK 11.46)
  - build2: KSU 35008. **Gravado → bootloop** (logo POCO, reboot rápido, nada no pstore)
  - build3: testou a hipótese do DTB. Os DTBs eram idênticos, então essa não era a causa
- A causa do bootloop não foi achada.

**Por que foi abandonada:** carregando o blob r49 direto sobre o kernel **stock** (r32p1), só
apareceu um aviso de versão (UK 11.46 vs 11.38). O kernel novo não era necessário. O bloqueio real
era o blob ser de G52.

## Fase 2: blob r49 sobre o kernel stock

1. Probe direto da HAL: `DDK built for 0x7000002 ... not compatible` → o blob donor é de G52.
2. Patch da checagem de GPU ID → **Mali-G76, Vulkan 1.3.278**. Compute correto.
3. Módulo `vk49_g76` (v1): só Vulkan, com o GLES no r32p1.
4. Os apps continuavam no r32: o "Umount modules" do perfil do app desmontava o módulo. Desligado →
   o app passou a usar o r49.
5. **Mesmo assim falhava**: 32 pipelines com `VK_ERROR_INITIALIZATION_FAILED`, igual ao r32.

## Fase 3: causa real e vkshim

- O dump dos pipelines que falhavam mostrou SPIR-V com `RuntimeDescriptorArray`
  (`SPV_EXT_descriptor_indexing`). A G76 tem `runtimeDescriptorArray = 0` em qualquer driver.
- Fizemos o vkshim (HAL wrapper) para converter esses arrays para tamanho fixo.
- **Resultado (2026-11-24):** o port passou a renderizar em Vulkan na Mali-G76.
- Testes avançados de validação: veja a seção *Validação* do README.

## Fase 4: acabamento (2026-11-25)

- Confirmado depois de reboot: loader do sistema → vkshim → r49, Vulkan 1.3.278, recursos 1.3
  (`dynamicRendering`, `synchronization2`, `maintenance4`) ativos, `vkCreateDevice` OK.
- vkshim v2: logs e dumps só com `debug.vkshim=1`, sem custo no uso normal.
- Módulo v2, repositório reorganizado e build reproduzível (`tools/make_module.sh`).

## O que ficou de fora

- Kernel r49p1: não é necessário. O kit está no histórico do git (commits até `71c1943`).
- GLES r49: não testado como driver do sistema; o r32p1 segue estável para a interface.

## Fase 5: v4 (2026-11-26)

- Pesquisa sobre o que falta no r49 da G76 e como outros projetos contornam isso no Mali de
  fábrica (wrapper e fork do SPIRV-Tools do leegao, PixelBC). Resumo em [PESQUISA-V4.md](PESQUISA-V4.md).
- Teste de ponta a ponta (`tools/probe/v4test`) mostrou que, na v2, um shader com ClipDistance
  derrubava o driver. O passe do leegao não cobria o formato do glslang, então foi escrito o
  `clipcull.c`.
- Passes Mali do fork linkados no vkshim (libc++ estático para caber no namespace sphal);
  recursos emulados; cache de pipelines persistente; PixelBC sem perda encadeado.
- Pacote portátil (v3.0) mudou para o repositório público `mali-g76-vulkan13-portable`, que
  também recebeu a v4.
