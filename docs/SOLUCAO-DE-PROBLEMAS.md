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
- Layouts com `descriptorCount` maior que 8192/256: ajuste `vkshim.cfg` (`N_IMG N_SMP N_OTHER`).

Para testar o patch SPIR-V sem o aparelho rodando o jogo:
```sh
clang -O2 -o spvtest shim/spvtest.c -ldl -llog
./spvtest entrada.spv saida.spv && spirv-val saida.spv
```

## Voltar ao estado stock

Remova o módulo no gerenciador de root e reinicie. O módulo não muda nada fora do overlay de
`/vendor`.

## Launcher recusa o driver ZIP ("Unknown driver family" / "families do not match")

Launchers estilo adrenotools classificam o pacote pelo texto de `backend`, `name` e `libraryName`
do `meta.json` (procuram "turnip", "panvk", "radv", "pvr"…). Um pacote só com "Mali r49" cai como
família **Unknown** e é recusado. Use o **`vk49_g76-v3.0-portatil.zip`** (já vem com
`backend: panvk`, `panvkFrontend: bifrost-jm`, `gpuModels: [G76]`).

Outro erro comum: importar **só** o `vulkan.mt6785.so` do módulo. Ele depende do HAL
`/vendor/lib64/hw/vulkan.r49.so`, que não fica visível no namespace do driver importado; o pacote
portátil traz o HAL e o blob junto.

Se o driver aparecer mas falhar ao carregar, veja `logcat -s vkshim`: a linha
`vkshim v2 ativo sobre …` diz de onde o HAL foi aberto.
