#pragma once
// spvemu: reescritas de SPIR-V feitas na criação do pipeline, quando os dois lados da
// interface (último estágio antes da rasterização -> fragment shader) são conhecidos.
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int model;          /* ExecutionModel do entry point (0 VS, 1 TCS, 2 TES, 3 GS, 4 FS, 5 CS); -1 = não achou */
    int clip_n;         /* tamanho do gl_ClipDistance de saída que o shader escreve (0 = não escreve) */
    int has_position;   /* escreve gl_Position (saída) */
    int has_drawindex;  /* lê gl_DrawID (BuiltIn DrawIndex) */
    int out_loc_end;    /* primeira location livre depois das saídas do usuário */
    int in_loc_end;     /* primeira location livre depois das entradas do usuário */
} SpvInfo;

/* 0 = ok; -1 = módulo inválido ou entry point não achado */
int spvemu_info(const uint32_t *w, size_t nw, const char *entry, SpvInfo *inf);

/* Último estágio antes da rasterização (VS, TES ou GS):
 *  clip_loc >= 0 : o gl_ClipDistance de saída vira varying vec4 em clip_loc.. (devolve nvec em *clip_nvec);
 *                  o built-in original continua declarado (o clipcull retira depois)
 *  clamp         : z de gl_Position preso a [0, w] em cada vértice (depthClamp)
 *  drawid_off>=0 : leituras de gl_DrawID passam a vir de um push constant int no offset dado
 * Devolve NULL se nada foi feito ou se algum uso não é reconhecido (o chamador segue sem a emulação). */
uint32_t *spvemu_prerast(const uint32_t *w, size_t nw, const char *entry, int clip_loc, int clamp,
                         int drawid_off, size_t *out_nw, int *clip_nvec, unsigned *done);
enum { SPVEMU_DID_CLIP = 1, SPVEMU_DID_CLAMP = 2, SPVEMU_DID_DRAWID = 4 };

/* Fragment shader: lê as nvec varyings vec4 em loc.. e descarta o fragmento se algum
 * componente for negativo (mesmo conjunto de pixels que o recorte do hardware manteria). */
uint32_t *spvemu_fs_clip(const uint32_t *w, size_t nw, const char *entry, int loc, int nvec, size_t *out_nw);

/* Fragment shader mínimo (para pipelines sem fragment shader e sem escrita de cor). */
uint32_t *spvemu_fs_new(int loc, int nvec, size_t *out_nw);

/* Geometry shader que transforma triângulos em arestas (points = 0) ou vértices (points = 1) para
 * emular fillModeNonSolid, repassando as saídas do VS dado. cull: bit0 culla frente, bit1 culla verso,
 * bit2 front face horário, bit3 viewport com altura negativa (0 = sem culling). NULL se o VS tem saídas
 * que o gerador não sabe repassar. */
uint32_t *spvemu_gs_poly(const uint32_t *w, size_t nw, const char *entry, int points, int cull, size_t *out_nw);
