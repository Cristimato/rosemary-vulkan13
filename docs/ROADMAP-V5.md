# Roadmap v5.0 — de "compatível" para "liso"

Escrito em 2026-11-27, depois do primeiro teste em jogo real com a v4.7 (Dragon Rage / BT3).

Até a v4.7 o trabalho foi **compatibilidade**: Vulkan 1.3, bindless, clip, depthClamp, erratas.
A G76 continua com a mesma potência (MFGPLL travado em ~955 MHz). Então o ganho da v5 não vem
de "mais GPU": vem de **gastar menos GPU por quadro** e de **entregar os quadros com ritmo
estável**. O teste no BT3 mostrou isso: 30 fps em 1x, 15 em 2x e 11 em 3x, igual nos dois
drivers. O jogo está preso no fillrate (pixels), não no driver.

## 1. O que ficou errado ou pela metade

| # | Problema | Situação | Ação na v5 |
|---|---|---|---|
| E1 | **Chão transparente perto da câmera no BT3** | Novo. Suspeita: depthClamp por vértice achata o z de primitivas que cruzam o near (limitação documentada na v4.7). | A/B com `debug.vkshim.clamp 0`, depois **depthClamp exato** (item P0.1) |
| E2 | v4.7 publicada **sem teste em jogo real** | Aconteceu de novo (v4 e v4.7) | Matriz mínima de jogos antes de cada release (item Q1) |
| E3 | "Umount modules" tem que ser desligado à mão em **cada jogo** (KSU) | Todo jogo novo cai no r32 sem avisar | Automatizar ou ao menos detectar e avisar (item U1) |
| E4 | Props `debug.vkshim.*` são **globais** | Um ajuste para um jogo vale para todos | Config **por pacote** (item U2) |
| E5 | PixelBC sempre encadeado | Custa ~0,25 µs por `vkCmdPushConstants` mesmo em jogo sem BC | Ligar só quando o app criar imagem BC (item P1.4) |
| E6 | `shaderCullDistance` só anunciado | Geometria a mais, nunca a menos | Emular por discard (baixa prioridade) |
| E7 | v3.0 portátil ficou mais lenta que o r32 no COD4 e a causa nunca foi achada | Não se sabe se a v4/v4.7 resolveram | Medir com o HUD (P0.2) |
| E8 | Repos **públicos** com o blob proprietário ARM/Xiaomi dentro dos zips | Risco de DMCA/remoção | Decidir: zip só com o shim + script que monta o pacote a partir do donor, ou manter (decisão do usuário) |
| E9 | Docs prometiam recurso "suportado" que era só anunciado (v4) | Corrigido na v4.7 | Manter tabela "nativo × emulado × ignorado" em toda release |

## 2. Desempenho: onde está o ganho (em ordem de retorno esperado)

### P0 — antes de otimizar, medir e corrigir

**P0.1 depthClamp exato (corrige E1).** Em vez de prender o z em cada vértice:
- VS: manda `z_clip` e `w_clip` como varyings e escreve `gl_Position.z = 0.5*w` (nada é cortado no near/far);
- FS: `gl_FragDepth = clamp(z/w, minDepth, maxDepth)`.
Com interpolação perspective-correct, `z/w` no FS é a profundidade real. O custo é perder o early-Z, mas só nos pipelines que pedem `depthClampEnable`.

**P0.2 HUD/telemetria do vkshim (`debug.vkshim.hud` ou por pacote).** É a peça que faltava para saber o que fazer em cada jogo:
- tempo de quadro (CPU) entre `vkAcquireImageANDROID` e `vkQueueSignalReleaseImageANDROID`;
- tempo de GPU por timestamp query no início/fim de cada submit (`vkQueueSubmit` interceptado);
- contadores do shim: pipelines criados e compilados na hora (stutter), draws, binds, BC decodificados;
- frequência da GPU (`/proc/gpufreq` ou ged) e temperatura amostradas;
- saída em log CSV na pasta do jogo (`files/vkshim_hud.csv`) e resumo no logcat a cada 5 s.
Com isso dá para separar **GPU-bound × CPU-bound × stutter de shader × throttling térmico**.

### P1 — ganhos grandes, opt-in por jogo

**P1.1 Limitador de FPS com ritmo estável (`fps=30`).** Na v4.7 o BT3 em 1x varia de 26 a 30. Travar em 30 com intervalo constante parece **mais liso** que 26–35 oscilando, e esquenta menos (menos throttling depois de 10 min). O shim já recebe `vkAcquireImageANDROID` e `vkQueueSignalReleaseImageANDROID` (são da HAL), então dá para dormir até o próximo slot de 33,3 ms. Não precisa de layer.

**P1.2 Escala de resolução com upscale grátis pelo HWC (`scale=0.75`).** O teste do BT3 mostra fps quase proporcional ao número de pixels. No Android, uma swapchain menor que a janela é esticada pelo compositor de hardware sem custo de GPU. O problema é que `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` é implementada pelo `libvulkan.so` do sistema, não pela HAL. Opções, em ordem de risco:
1. jogos com opção de resolução própria: só documentar (BT3 já tem);
2. hook dentro do processo na função de caps do `libvulkan.so` (o shim já roda no processo), reportando uma `currentExtent` menor;
3. downscale interno: render pass na swapchain redirecionado para uma imagem menor e blit no fim (mais invasivo).
**Precisa de prova de conceito** antes de entrar.

**P1.3 Shaders em FP16 (`fp16=1`, experimental).** A Bifrost faz FP16 com o dobro da vazão do FP32. Um passe SPIR-V marca `RelaxedPrecision` nas operações float do **fragment shader**, exceto coordenadas de textura, profundidade e o que alimenta `discard`. Num jogo preso em fillrate como o BT3, é o maior ganho possível sem baixar resolução. Risco: banding e texturas tremendo. Por isso fica opt-in por jogo e é validado com captura de tela.

**P1.4 Amostragem mais barata (`aniso=2`, `lodbias=0.5`).** Interceptar `vkCreateSampler` para limitar `maxAnisotropy` (aniso 16x é caro no Mali) e somar um bias de LOD. Fácil, seguro, opt-in.

**P1.5 PixelBC preguiçoso (corrige E5).** Só encadear o PixelBC quando o app criar a primeira imagem BC1–BC7. Jogo sem BC fica sem custo nenhum.

### P2 — ganhos de banda (Mali é tile-based)

**P2.1 Attachments transitórios.** Depth/stencil e MSAA com `storeOp = DONT_CARE` e uso só de attachment podem ir para memória lazily-allocated, que mora no tile e não gasta banda. Interceptar `vkCreateImage`/`vkAllocateMemory` para isso é seguro **só** quando o uso é exclusivamente de attachment. Antes de mexer, medir com o HUD.

**P2.2 Preservar AFBC.** Imagens criadas com `STORAGE_BIT` perdem compressão AFBC. Precisa de pesquisa: listar os usages que os jogos pedem (dump no HUD) antes de pensar em tirar flags.

**P2.3 Aquecer o cache de pipelines.** O pcache persistente já existe. Dá para compilar em segundo plano, numa thread, as variantes já vistas (clip/clamp/GS) logo no `vkCreateDevice`, para tirar o stutter do primeiro uso.

### P3 — sistema (fora do driver, mas faz parte do "liso")
- Integrar com o Game Mode existente: o shim grava `debug.vkshim.active=<pkg>` e o daemon trava a GPU em 955 MHz, prende o jogo nos núcleos grandes e ajusta o térmico dentro de margem segura. Hoje o DVFS do ged pode derrubar a frequência no meio da luta. Isso só vale para o KSU; no portátil não há root.
- Medir com o HUD se a GPU está mesmo em 955 durante o jogo. Se não estiver, esse é o ganho mais barato de todos.

## 3. Usabilidade

| # | Item |
|---|---|
| U1 | Detectar se o app está vendo o r32 (namespace sem o módulo) e avisar por log/toast; pesquisar se o `ksud` do SukiSU permite desligar "Umount modules" por pacote via CLI e, se der, fazer isso no `service.sh` para uma lista de jogos. |
| U2 | Config por pacote: `/data/adb/vkshim/<pacote>.conf` (KSU) e `vkshim.conf` na pasta do driver (portátil). Chaves: `fps scale fp16 aniso lodbias clip clamp drawid nonsolid errata bcn hud`. As props globais continuam como padrão. |
| U3 | `updateJson` no `module.prop` para o KSU avisar de update. |
| U4 | Perfis prontos: `liso` (fps=30, aniso=2), `desempenho` (+fp16, lodbias), `qualidade` (nada). |

## 4. Qualidade e release

- **Q1 Matriz de jogos** antes de publicar: BT3/Dragon Rage, FH4Droid, COD4 (portátil) e mais um port Vulkan. Em cada um: abre, 5 min jogando, sem crash, sem glitch novo, e linha do HUD salva.
- **Q2** O probe `e47` cresce com um caso para cada bug achado em jogo (o chão do BT3 vira `clamp_cruza_near`).
- **Q3** Os dois zips (KSU e portátil) saem do mesmo commit, com o mesmo número de versão.

## 5. Ordem sugerida

1. **v4.8 (correção):** P0.1 depthClamp exato, P1.5 PixelBC preguiçoso, caso novo no e47, teste no BT3.
2. **v4.9 (medição):** P0.2 HUD, U2 config por pacote. Medir BT3/FH4/COD4 e registrar os números.
3. **v5.0 (desempenho):** P1.1 limitador, P1.4 samplers, P1.3 fp16 experimental e P1.2 escala (se a prova de conceito passar), perfis U4, integração com o Game Mode, matriz Q1. Release dos dois zips.
