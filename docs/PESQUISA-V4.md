# Pesquisa e decisões da v4

Este documento resume o que foi investigado para a v4, o que falhava na v2 e por que cada
correção foi feita do jeito que foi.

## 1. O que o driver r49p1 informa na Mali-G76

Medido no aparelho com `tools/probe/fullvk.c` (Vulkan 1.3.278, 126 extensões). Recursos
que importam para ports de PC e que o driver informa como **ausentes**:

| Recurso | Situação | Impacto típico |
|---|---|---|
| `textureCompressionBC` (BC1–BC7) | nenhum formato BC | engines de PC usam BC para quase todas as texturas |
| `runtimeDescriptorArray`, `descriptorIndexing`, `partiallyBound`, `variableDescriptorCount` | 0 | renderizadores bindless (já tratado na v1/v2) |
| `shaderClipDistance`, `shaderCullDistance` | 0 | planos de recorte; na v2 um shader com ClipDistance **derrubava o driver** |
| `multiDrawIndirect`, `drawIndirectCount` | 0 (`maxDrawIndirectCount` = 1) | renderização GPU-driven |
| `depthClamp`, `fillModeNonSolid`, `multiViewport`, `depthBounds` | 0 | sombras, debug/wireframe, VR |
| `vertexPipelineStoresAndAtomics` | 0 | escrita em buffer no vertex shader |
| `samplerFilterMinmax`, `shaderFloat64`, sparse | 0 | menos comuns |

O blob não tem ajustes de desempenho por propriedade: as `debug.gpu.*` que ele lê são de
depuração da MediaTek.

## 2. Quem já resolveu isso no Mali de fábrica

- **leegao/bionic-vulkan-wrapper** (usado no Winlator): finge recursos que faltam no Mali e
  saneia o estado dos pipelines. A lista bate com a nossa: depthClamp, fillModeNonSolid,
  clip/cull distance, multiDrawIndirect, vertexPipelineStoresAndAtomics.
- **leegao/SPIRV-Tools** (fork): passes que contornam bugs do compilador Mali:
  - `FixMaliSpecConstantComposite`: vetores bool feitos de spec constants viram constantes comuns;
  - `MaliOptimizationBarrier`: constant folding errado de shifts;
  - `MaliBitwiseScalar`: erratum Arm 3843237 (r18p0–r53p0, **inclui o r49**): operações
    bitwise/shift vetoriais com swizzle ou vetor montado lane a lane;
  - `MaliClampZero`: erratum Arm 3779215: clamp/min/max com limite 0;
  - `MaliLockstep`: loops de compute escritos para wave de 32 (a G76 tem warp de 8);
  - `RemoveClipCullDist`: só cobre variáveis soltas (formato Zink), não o bloco do glslang.
- **supplefrog/PixelBC**: wrapper de HAL para o Mali de fábrica que decodifica BC1–5/BC7 na
  GPU por compute. Tem perfil com ETC2 (com perda) e sem perda (RGBA).

## 3. O que a v4 faz com cada um

| Problema | Solução na v4 | Fidelidade |
|---|---|---|
| ClipDistance/CullDistance derrubando o driver | `shim/clipcull.c` (nosso): remove os stores, troca loads por zero, corta os membros do `gl_PerVertex`, tira capabilities. Cobre o formato do glslang e variáveis soltas | os planos de recorte são ignorados (nada é recortado) |
| Bugs do compilador Mali | passes do fork leegao, só quando a varredura acha o padrão | correção, sem mudança visual |
| depthClamp / fillModeNonSolid / multiViewport | anunciados; o pipeline é saneado (clamp off, polígono FILL, 1 viewport), também no estado dinâmico | aproximado |
| multiDrawIndirect | anunciado; `vkCmdDraw(Indexed)Indirect` com N comandos vira N draws | exato, exceto `gl_DrawID` = 0 |
| Texturas BC | PixelBC sem perda encadeado (`vulkan.pixelbc.so`); formatos BC aparecem individualmente | exato (sem perda) |
| Stutter de compilação | cache de pipelines persistente por app | — |
| vertexPipelineStoresAndAtomics, descriptor indexing completo | **opcionais** (`debug.vkshim.fake`), desligados por padrão | não emulados |

Por que alguns ficam desligados: fingir um recurso sem emulá-lo faz o jogo escolher o caminho
que usa o recurso, e esse caminho quebra. Sem o recurso anunciado, o jogo usa o caminho
alternativo que funciona.

O perfil ETC2 do PixelBC foi descartado: no G76 ele deu 32 diferenças de pixel em BC1/BC3.
O perfil sem perda passou em tudo.

## 4. Validação

| Teste | Resultado |
|---|---|
| `tools/probe/v4test` na **v2**: pipeline com ClipDistance | ❌ SIGSEGV dentro do driver |
| `tools/probe/v4test` na **v4**: device com recursos emulados, pipeline com ClipDistance (1 e 2 planos) + depthClamp + LINE, compute, `vkCmdDrawIndirect(drawCount=4)` executado na GPU | ✅ tudo OK |
| `v4test` com `debug.vkshim.fake=all` e `=none` | ✅ OK nos dois |
| Cache de pipelines: 1ª execução grava, 2ª carrega e valida o cabeçalho | ✅ |
| `tests/run.sh`: clip/cull e passes Mali em shaders de teste, `spirv-val` | ✅ válidos, sem Clip/CullDistance |
| Passes Mali em 16 shaders reais de um port (Xenia) | ✅ 14 alterados, 0 erros, 0,85 ms/shader, todos válidos |
| PixelBC sem perda, probe principal (BC1–BC5, uploads, cópias, rejeições) | ✅ 535 / 0 |
| PixelBC sem perda, probe BC7 | ✅ 20046 / 0 |
| PixelBC perfil ETC2 | ⚠️ 503 / 32 (perda de cor esperada; não usado) |
| Cadeia shim v4 → PixelBC → r49 no `v4test` | ✅ |
| Build reproduzível: `make_module.sh` comparado com o pacote testado | ✅ idêntico (o PixelBC difere só pelo patch de caminho, revalidado 535/0 e 20046/0) |

## 5. Limites

- A v4 não deixa a GPU mais rápida. O ganho é compatibilidade (jogos que falhavam ou derrubavam
  o driver) e menos travadas por compilação a partir da segunda vez que o jogo abre.
- `drawIndirectCount`, geometry streams, sparse e float64 continuam sem solução.
- Ainda falta teste dentro de jogos reais com a v4.

## Fontes

- https://github.com/leegao/bionic-vulkan-wrapper (issue #27: extensões que faltam no Mali)
- https://github.com/leegao/SPIRV-Tools (README do fork descreve cada passe e os erratas)
- https://github.com/supplefrog/PixelBC
- https://github.com/FristOneRR-Admin/FristOneRR-Wrapperv1 e WinlatorMali (uso das mesmas técnicas)
- Arm: *Mali Bifrost usage recommendations for texture and sampler descriptors*;
  *Vulkan synchronization: how pipeline barriers map to Mali GPU hardware*
