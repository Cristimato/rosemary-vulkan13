# Solução de problemas

## O jogo continua no driver antigo

Sintoma: `Vulkan 1.1`, crash em `libGLES_mali.so` (e não em `libGLES_ma49.so`).

1. No KernelSU/SukiSU: *App Profile* do jogo → **desligar "Umount modules"**.
2. Confira o que o processo enxerga:
   ```sh
   su -c "nsenter -t $(pidof <pacote>) -m ls -la /vendor/lib64/hw/vulkan.mt6785.so"
   # certo: arquivo de ~21 KB    errado: symlink -> egl/libGLES_mali.so
   ```

## Conferir o driver no sistema

```sh
clang -O1 -o sysvk tools/probe/sysvk.c -ldl
su -c ./sysvk
```
Esperado:
```
GPU: Mali-G76 | API 1.3.278
1.3 feats: dynamicRendering=1 sync2=1 maintenance4=1
vkCreateDevice=0
  lib: /vendor/lib64/libGLES_ma49.so
  lib: /vendor/lib64/hw/vulkan.r49.so
  lib: /vendor/lib64/hw/vulkan.mt6785.so
```
Os avisos `user 11.46, kernel 11.38` e `large_page_conf` são normais.

## Testar um vkshim novo sem reiniciar

```sh
clang -O1 -o shimvk tools/probe/shimvk.c -ldl
su -c "LD_LIBRARY_PATH=/vendor/lib64/hw:/vendor/lib64:/vendor/lib64/mt6785 ./shimvk /data/local/tmp/novo_shim.so"
```

## Pipelines falhando num jogo novo

```sh
su -c setprop debug.vkshim 1
# abra o jogo e reproduza
su -c "logcat -d -s vkshim"
su -c "ls /data/user/0/<pacote>/cache/vkdump"
```
- `vkCreateGraphicsPipelines FALHOU -3`: veja `fail_NNN.txt` (estado) e o `.spv` (shader).
  Desmonte com `spirv-dis` e procure capabilities ou recursos que a G76 não tem (ex.: clip/cull
  distance, `depthClamp` e `fillModeNonSolid` são 0 nesta GPU).
- Layouts com `descriptorCount` maior que 8192/256: os limites são fixos em `shim/shim.c`
  (`N_IMG`, `N_SMP`, `N_OTHER`); é preciso recompilar.

Para conferir erratas e emulações na GPU: `tools/probe/e47/build.sh` e rode o `e47` contra a HAL.
Para testar os passes de SPIR-V sem rodar o jogo: `tests/run.sh` (shaders de teste) ou
compile `tests/spvtest.cpp` / `tests/cctest.c` e passe os `.spv` despejados.

## Isolar um problema

Cada parte do vkshim pode ser desligada sem reinstalar (reabra o jogo depois):

```sh
su -c setprop debug.vkshim.bcn 0       # tira o PixelBC (texturas BC)
su -c setprop debug.vkshim.spv 0       # desliga os passes Mali de SPIR-V
su -c setprop debug.vkshim.pcache 0    # desliga o cache de pipelines
su -c setprop debug.vkshim.fake none   # não anuncia nenhum recurso emulado
su -c setprop debug.vkshim.clip 0      # v4.7: recorte por discard desligado (planos ignorados)
su -c setprop debug.vkshim.clamp 0     # v4.7: depthClamp desligado
su -c setprop debug.vkshim.drawid 0    # v4.7: gl_DrawID por push constant desligado
su -c setprop debug.vkshim.nonsolid 0  # v4.7: wireframe por geometry shader desligado
su -c setprop debug.vkshim.errata 0    # v4.7: correções das erratas do driver desligadas
```

Se o jogo só funciona com uma delas desligada, abra uma issue com o `logcat -s vkshim`.
Para apagar o cache de um jogo: `rm /data/user/0/<pacote>/cache/vkshim_pcache.bin`.

## Voltar ao estado stock

Remova o módulo no gerenciador de root e reinicie. O módulo não muda nada fora do overlay de
`/vendor`.
