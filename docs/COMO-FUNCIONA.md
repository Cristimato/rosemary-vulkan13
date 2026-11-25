# Como funciona

## 1. Por que um blob de outra GPU

| | rosemary (alvo) | donor |
|---|---|---|
| SoC | MT6785 (Helio G95) | MT6768 (Helio G85) |
| GPU | Mali-G76 MC4, GPU ID `0x7211` | Mali-G52, GPU ID `0x7000002` |
| Arquitetura | Bifrost v7 | Bifrost v7 |
| Blob | r32p1 (Vulkan 1.1) | r49p1-03bet0 (Vulkan 1.3) |

Procuramos um blob r4x nativo de G76 e não achamos: os aparelhos com G76 pararam no r3x. Os
projetos "malisx2"/NX-Mali/PanVK só funcionam dentro de emuladores/launchers, não no sistema todo.

O DDK r49 é compilado para uma GPU específica e recusa as outras. Ao carregar direto na G76:

```
DDK built for 0x7000002 ... not compatible, detected 0x7020101
```

Como G52 e G76 têm o mesmo ISA, forçamos a checagem a passar.

## 2. Patches binários (`tools/patch_blob.py`)

| Arquivo | Offset | Original | Novo | Motivo |
|---|---|---|---|---|
| `egl/libGLES_mali.so` | `0x2490E` | `li` | `49` | SONAME `libGLES_mali.so` → `libGLES_ma49.so` |
| `egl/libGLES_mali.so` | `0x1F29624` | `cmp w26,w27; b.ne` | `mov w0,#1; b +0xb8` | aceita qualquer GPU ID Bifrost |
| `hw/vulkan.mali.so` | `0x388` | `li` | `49` | NEEDED → `libGLES_ma49.so` |

O renome de mesmo tamanho dispensa realocar `.dynstr`. Com isso, `libGLES_mali.so` (r32p1, usado
pelo EGL/HWUI do sistema) e `libGLES_ma49.so` (r49p1, só Vulkan) carregam juntos, até no mesmo
processo. Isso foi testado num binário com GLES r32 e Vulkan r49 ativos ao mesmo tempo.

`libgp49.so`, `libgpd9.so` e as libs AIDL `android.hardware.*-ndk` vêm do donor sem mudança.

## 3. Compatibilidade com o kernel stock

O kernel stock tem kbase r32p1 (UK 11.38). O blob r49 espera UK 11.46 e só avisa:

```
[WARN] file /dev/mali0 is different version (user 11.46, kernel 11.38)
```

As ioctls usadas pelo Vulkan na G76 são compatíveis. Shaders de compute dão resultado correto, com
a mesma velocidade do r32. `large_page_conf` não existe no kbase r32 e o blob segue sem ele.

## 4. vkshim (`shim/shim.c`)

### O problema

O port (Xenia, "shadowZero") cria descriptor sets bindless:
`OpTypeRuntimeArray` de `OpTypeImage`/`OpTypeSampler` com `SPV_EXT_descriptor_indexing`.
Na Mali-G76, `runtimeDescriptorArray = 0` (no r32 **e** no r49). O resultado:

```
vkCreateGraphicsPipelines = -3 (VK_ERROR_INITIALIZATION_FAILED)   x32 pipelines
→ vkCmdDraw sem pipeline válido → SIGSEGV em libGLES_mali.so
```

### A solução

O vkshim é uma HAL `hwvulkan` (`HMI`) chamada `vulkan.mt6785.so`, o nome que o loader procura via
`ro.board.platform`. Ele faz `dlopen` em `hw/vulkan.r49.so`, repassa tudo e intercepta:

- **`vkCreateShaderModule`**: reescreve o SPIR-V:
  - `OpTypeRuntimeArray<imagem>` → `OpTypeArray<imagem, 8192>`
  - `OpTypeRuntimeArray<sampler>` → `OpTypeArray<sampler, 256>`
  - remove `OpCapability` 5301/5302/5306–5312 (descriptor indexing), `OpExtension
    SPV_EXT_descriptor_indexing` e `OpDecorate NonUniform`
  - declara `OpTypeInt 32 0` e as constantes de tamanho quando necessário; atualiza o *bound*
  - runtime arrays de SSBO (structs) ficam como estão (são suportados)
- **`vkCreateGraphicsPipelines`**: registra falhas no logcat (até 20).

Os tamanhos podem ser ajustados sem recompilar, com um arquivo com três números
(`N_IMG N_SMP N_OTHER`) em `/vendor/lib64/hw/vkshim.cfg` ou `/data/local/tmp/vkhw_cfg`.

### Modo debug (`setprop debug.vkshim 1`)

Lido uma vez, quando a HAL é aberta. Liga:
- cópia de cada shader module para despejar o pipeline que falhar (SPIR-V + estado completo) em
  `/data/user/0/<pkg>/cache/vkdump/fail_NNN.txt|.spv`
- logs de `vkCreateDescriptorSetLayout`, `vkCreateDescriptorPool` e `vkAllocateDescriptorSets`

Desligado (padrão), não há cópia de SPIR-V nem log por chamada.

## 5. Módulo

```
system/vendor/lib64/hw/vulkan.mt6785.so   vkshim
system/vendor/lib64/hw/vulkan.r49.so      HAL r49p1
system/vendor/lib64/libGLES_ma49.so       blob r49p1 (patchado)
system/vendor/lib64/libgp49.so, libgpd9.so, android.hardware.*-ndk.so
system/vendor/etc/permissions/android.hardware.vulkan.version.xml   → 1.3 (4206592)
```

`customize.sh` só aceita `ro.board.platform=mt6785` e aplica o contexto SELinux
`same_process_hal_file`. Sem ele, o namespace `sphal` não carrega as libs.

### "Umount modules"

O KernelSU/SukiSU (com ReZygisk/BreZygisk) desmonta os módulos no namespace dos apps que têm
"Umount modules" ligado. Nesse caso, `vulkan.mt6785.so` volta a ser o symlink stock para o r32p1.
Para conferir:

```sh
su -c "nsenter -t $(pidof <pacote>) -m ls -la /vendor/lib64/hw/vulkan.mt6785.so"
```
