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

## ATUALIZAÇÃO: os 86 erros vêm da branch errada

A `main` do SukiSU-Ultra (1439545f) é para kernels GKI ou novos. Para **4.19 não-GKI**, o SukiSU mantém a branch
**`builtin`** (só a pasta `kernel/` + `uapi/`, mesmo layout; o último commit é "Fix 3.10 build and compile
out KPM/cpu_spoof below 5.10"):

```
origin/builtin  4bec532d 2026-11-16 bump Version Number   (4692 commits)
```

Troque no build.sh:
```
git -C src/KernelSU fetch origin builtin
git -C src/KernelSU checkout -q 4bec532d     # origin/builtin
```
- O symlink `drivers/kernelsu -> ../KernelSU/kernel` continua válido.
- O Kconfig da builtin não tem `KSU_MANUAL_HOOK` (o `-d` no scripts/config vira no-op, sem problema).
- Deixe `KPM`, `KSU_SUSFS*` e `KSU_FEATURE_ADBROOT` **desligados**.
- A versão sai como 30000 + 4692 - 4001 = 30691, diferente da 35008 do kernel atual. Tudo bem: o manager instalado
  (ksud 4.1.0) é o que vale, e a versão é só informativa. Se o manager reclamar depois, a gente ajusta no celular.
Se a builtin também der erro, mande as primeiras ~30 linhas do erro num arquivo `errors.txt` no repositório.
