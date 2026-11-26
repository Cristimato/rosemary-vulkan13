// clipcull: retira gl_ClipDistance/gl_CullDistance de módulos SPIR-V.
// O compilador Mali (Bifrost, r32/r49) não tem esses built-ins: com eles o
// vkCreateGraphicsPipelines derruba o driver (SIGSEGV). Este passe cobre o formato
// do glslang (membros do bloco gl_PerVertex) e variáveis soltas (formato Zink):
//   - stores nesses destinos são removidos; loads viram OpConstantNull;
//   - os membros (que precisam ser os últimos do struct) são cortados do OpTypeStruct;
//   - OpMemberDecorate/OpMemberName/OpDecorate/OpName/capabilities correspondentes saem.
// Efeito visual: planos de recorte do usuário são ignorados (nada é recortado/descartado).
// Qualquer uso não reconhecido => devolve NULL (o módulo segue inalterado).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "clipcull.h"

#define OP(w) ((w) & 0xffff)
#define WC(w) ((w) >> 16)

enum { K_NONE, K_STRUCT0, K_STRUCT1, K_DEADVAR };

uint32_t *clipcull_lower(const uint32_t *in, size_t nw, size_t *out_nw) {
    if (nw < 5 || in[0] != 0x07230203) return NULL;
    uint32_t bound = in[3];
    if (bound > (1u << 22)) return NULL;
    int any = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = WC(in[i]); if (!wc || i + wc > nw) return NULL;
        uint32_t op = OP(in[i]);
        if (op == 17 && (in[i+1] == 32 || in[i+1] == 33)) any = 1;
        if (op == 71 && wc >= 4 && in[i+2] == 11 && (in[i+3] == 3 || in[i+3] == 4)) any = 1;
        i += wc;
    }
    if (!any) return NULL;

    uint32_t *rmmask = calloc(bound, 4);   /* struct id -> bitmask de membros a remover */
    uint32_t *nmem   = calloc(bound, 4);   /* struct id -> nº de membros */
    uint32_t *ptee   = calloc(bound, 4);   /* ponteiro -> tipo apontado */
    uint32_t *elem   = calloc(bound, 4);   /* array -> elemento */
    int32_t  *cval   = malloc(bound * 4);  /* constante inteira -> valor (-1 = não) */
    uint8_t  *kind   = calloc(bound, 1);   /* variável -> K_* */
    uint32_t *kstr   = calloc(bound, 4);   /* variável -> struct alvo */
    uint8_t  *dead   = calloc(bound, 1);   /* ids de ponteiros mortos */
    uint32_t *nullof = calloc(bound, 4);   /* tipo -> id do OpConstantNull novo */
    uint32_t *out = NULL;
    if (!rmmask || !nmem || !ptee || !elem || !cval || !kind || !kstr || !dead || !nullof) goto fail;
    memset(cval, 0xff, bound * 4);

    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i]; uint32_t op = OP(w[0]), wc = WC(w[0]);
        if (op == 72 && wc >= 5 && w[3] == 11 && (w[4] == 3 || w[4] == 4) && w[1] < bound && w[2] < 32) rmmask[w[1]] |= 1u << w[2];
        else if (op == 71 && wc >= 4 && w[2] == 11 && (w[3] == 3 || w[3] == 4) && w[1] < bound) kind[w[1]] = K_DEADVAR;
        else if (op == 30 && w[1] < bound) nmem[w[1]] = wc - 2;
        else if (op == 32 && wc == 4 && w[1] < bound) ptee[w[1]] = w[3];
        else if ((op == 28 || op == 29) && w[1] < bound) elem[w[1]] = w[2];
        else if (op == 43 && wc == 4 && w[2] < bound) cval[w[2]] = (int32_t)w[3];
    }
    /* os membros removidos precisam ser os últimos do struct */
    for (uint32_t s = 0; s < bound; s++) {
        if (!rmmask[s]) continue;
        uint32_t n = nmem[s], m = rmmask[s];
        if (!n || n > 32) goto fail;
        int first = n; while (first > 0 && (m >> (first - 1) & 1)) first--;
        if ((m >> first) != ((1u << (n - first)) - 1) || (first < 32 && (m & ((1u << first) - 1)))) goto fail;
    }
    /* variáveis que apontam para esses structs */
    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i];
        if (OP(w[0]) != 59 || w[1] >= bound || w[2] >= bound) continue;
        uint32_t t = ptee[w[1]];
        if (kind[w[2]] == K_DEADVAR) { dead[w[2]] = 1; continue; }
        if (t < bound && rmmask[t]) { kind[w[2]] = K_STRUCT0; kstr[w[2]] = t; }
        else if (t < bound && elem[t] < bound && rmmask[elem[t]]) { kind[w[2]] = K_STRUCT1; kstr[w[2]] = elem[t]; }
    }
    /* access chains: marca os que entram nos membros removidos */
    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i]; uint32_t op = OP(w[0]), wc = WC(w[0]);
        if ((op != 65 && op != 66) || wc < 4 || w[2] >= bound || w[3] >= bound) continue;
        uint32_t base = w[3];
        if (dead[base]) { dead[w[2]] = 1; continue; }
        uint32_t k = kind[base];
        if (k != K_STRUCT0 && k != K_STRUCT1) continue;
        uint32_t pos = (k == K_STRUCT0) ? 0 : 1;
        if (wc < 5 + pos) continue;                 /* ainda não chegou ao membro */
        uint32_t ix = w[4 + pos];
        if (ix >= bound || cval[ix] < 0) goto fail;  /* índice de membro tem que ser constante */
        if (cval[ix] < 32 && (rmmask[kstr[base]] >> cval[ix] & 1)) dead[w[2]] = 1;
    }
    /* usos dos ponteiros mortos: só OpStore/OpLoad/OpName/OpDecorate/OpEntryPoint são aceitos */
    size_t nnull = 0;
    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i]; uint32_t op = OP(w[0]), wc = WC(w[0]);
        if (op == 62 && wc >= 3) continue;                              /* OpStore ptr obj */
        if (op == 61 && wc >= 4) { if (w[3] < bound && dead[w[3]] && !nullof[w[1]]) { nullof[w[1]] = 1; nnull++; } continue; }
        if (op == 5 || op == 71 || op == 15 || op == 59 || op == 65 || op == 66) continue;
        if (op == 72 || op == 6) continue;
        if (op < 54 && op != 57) continue;                              /* seção de tipos/constantes */
        for (uint32_t k = 1; k < wc; k++) if (w[k] < bound && dead[w[k]]) goto fail;
    }
    /* OpStore cujo valor é um ponteiro morto também não é aceito */
    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i];
        if (OP(w[0]) == 62 && WC(w[0]) >= 3 && w[2] < bound && dead[w[2]]) goto fail;
    }

    out = malloc((nw + 3 * nnull + 16) * 4);
    if (!out) goto fail;
    memcpy(out, in, 5 * 4);
    size_t o = 5; uint32_t next = bound; int nulls_done = 0;
    for (size_t i = 5; i < nw; i += WC(in[i])) {
        const uint32_t *w = &in[i]; uint32_t op = OP(w[0]), wc = WC(w[0]);
        if (op == 54 && !nulls_done) {                 /* antes da 1ª função: OpConstantNull */
            for (uint32_t t = 0; t < bound; t++) if (nullof[t]) { nullof[t] = next++; out[o++] = (3u << 16) | 46; out[o++] = t; out[o++] = nullof[t]; }
            nulls_done = 1;
        }
        if (op == 17 && (w[1] == 32 || w[1] == 33)) continue;
        if ((op == 72 || op == 6) && w[1] < bound && w[2] < 32 && (rmmask[w[1]] >> w[2] & 1)) continue;
        if ((op == 71 || op == 5) && w[1] < bound && dead[w[1]]) continue;
        if (op == 59 && w[2] < bound && dead[w[2]]) continue;
        if ((op == 65 || op == 66) && w[2] < bound && dead[w[2]]) continue;
        if (op == 62 && w[1] < bound && dead[w[1]]) continue;
        if (op == 61 && w[3] < bound && dead[w[3]]) {   /* load morto -> OpCopyObject do nulo */
            out[o++] = (4u << 16) | 83; out[o++] = w[1]; out[o++] = w[2]; out[o++] = nullof[w[1]]; continue;
        }
        if (op == 30 && w[1] < bound && rmmask[w[1]]) {
            uint32_t keep = 0; while (keep < nmem[w[1]] && !(rmmask[w[1]] >> keep & 1)) keep++;
            out[o++] = ((2 + keep) << 16) | 30; out[o++] = w[1];
            for (uint32_t k = 0; k < keep; k++) out[o++] = w[2 + k];
            continue;
        }
        if (op == 15) {                                  /* OpEntryPoint: tira variáveis mortas da interface */
            size_t start = o; out[o++] = 0; out[o++] = w[1]; out[o++] = w[2];
            uint32_t k = 3; while (k < wc) { out[o++] = w[k]; if ((w[k] >> 24) == 0 || (w[k] & 0xff000000) == 0) { k++; break; } k++; }
            /* o nome termina na palavra cujo byte alto é 0 */
            for (; k < wc; k++) if (!(w[k] < bound && dead[w[k]])) out[o++] = w[k];
            out[start] = ((uint32_t)(o - start) << 16) | 15;
            continue;
        }
        memcpy(&out[o], w, wc * 4); o += wc;
    }
    out[3] = next;
    *out_nw = o;
    free(rmmask); free(nmem); free(ptee); free(elem); free(cval); free(kind); free(kstr); free(dead); free(nullof);
    return out;
fail:
    free(out);
    free(rmmask); free(nmem); free(ptee); free(elem); free(cval); free(kind); free(kstr); free(dead); free(nullof);
    return NULL;
}
