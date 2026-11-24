# Resposta: qual SukiSU usar

Saída de `~/oc_work/vk/sukisu` no celular (é um repositório git, NÃO uma cópia de pasta):

```
1439545f12bffe0263690ba862139b70ee5f20cf 2026-11-17 manager: enhance module flashing screen (tiann/KernelSU#3998)
origin  https://github.com/SukiSU-Ultra/SukiSU-Ultra   (clone partial, blob:none, NÃO shallow)
git status --short: vazio (nenhuma alteração local)
git rev-list --count HEAD: 4008
```

Conclusão: use exatamente o que o `build.sh` já faz, ou seja, clone oficial de
https://github.com/SukiSU-Ultra/SukiSU-Ultra com checkout em 1439545f, histórico completo.
Não existe patch local. Você está certo que o fork Pietro-Maltose não tem SukiSU. Ele é integrado
pelo patch 0001 (symlink KernelSU + drivers/kernelsu + drivers/Makefile/Kconfig), e o build.sh
troca o symlink pelo clone real. Pode seguir com o build.
