# Componentes de terceiros

| Componente | Origem | Licença | Uso |
|---|---|---|---|
| Blob Mali r49p1 (libGLES_mali, vulkan.mali, libgpud) | ARM / MediaTek, via pacote *Mali-r49p1-MT6768* de Dracen-W | proprietária | driver Vulkan (só na release, não no código) |
| SPIRV-Tools (fork leegao, commit `20a5b43`) | https://github.com/leegao/SPIRV-Tools | Apache-2.0 | passes contra bugs do compilador Mali, linkados estaticamente no vkshim |
| SPIRV-Headers (`97e96f9`) | https://github.com/KhronosGroup/SPIRV-Headers | MIT-style (Khronos) | headers do SPIRV-Tools |
| PixelBC (commit `e994ccf` + `patches/pixelbc-backend-path.patch`) | https://github.com/supplefrog/PixelBC | MIT | decodificação de texturas BC1-5/BC7 na GPU (`vulkan.pixelbc.so`) |
| bcdec (dentro do PixelBC) | https://github.com/iOrange/bcdec | MIT | referência de decodificação BC |
| libc++ (estática) | LLVM | Apache-2.0 with LLVM exception | runtime C++ do vkshim e do PixelBC |

As licenças completas estão nos repositórios de origem. O vkshim, o `clipcull.c`, os scripts e a
documentação deste repositório são do projeto.
