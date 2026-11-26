// teste offline do spvemu + clipcull: gera variantes e grava <arq>.<sufixo>.spv para o spirv-val
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shim/spvemu.h"
#include "../shim/clipcull.h"
static uint32_t *rd(const char *p, size_t *nw) { FILE *f = fopen(p, "rb"); if (!f) { printf("sem %s\n", p); exit(1); } fseek(f, 0, 2); long l = ftell(f); fseek(f, 0, 0); uint32_t *b = malloc(l); if (fread(b, 1, l, f) != (size_t)l) exit(1); fclose(f); *nw = l / 4; return b; }
static void wr(const char *p, const char *suf, const uint32_t *w, size_t nw) { char o[600]; snprintf(o, sizeof o, "%s.%s.spv", p, suf); FILE *f = fopen(o, "wb"); fwrite(w, 4, nw, f); fclose(f); }
int main(int c, char **v) {
    int fails = 0;
    for (int i = 1; i < c; i++) {
        size_t nw; uint32_t *w = rd(v[i], &nw);
        SpvInfo inf;
        if (spvemu_info(w, nw, "main", &inf)) { printf("%s: info falhou\n", v[i]); fails++; continue; }
        printf("%-34s model=%d clip_n=%d pos=%d drawid=%d out_end=%d in_end=%d\n", v[i], inf.model, inf.clip_n, inf.has_position, inf.has_drawindex, inf.out_loc_end, inf.in_loc_end);
        { size_t ng; uint32_t *g = inf.model != 0 ? spvemu_gs_poly(w, nw, "main", 0, 0, &ng) : NULL; if (g) { printf("  gs de não-VS deveria falhar\n"); fails++; free(g); } }
        { size_t ng; uint32_t *g = spvemu_gs_poly(w, nw, "naoexiste", 0, 0, &ng); if (g) { fails++; free(g); } }
        if (inf.model == 4) {
            size_t n2; uint32_t *f = spvemu_fs_clip(w, nw, "main", inf.in_loc_end, 2, &n2);
            if (!f) { printf("  fs_clip FALHOU\n"); fails++; } else { wr(v[i], "fsclip", f, n2); free(f); }
            continue;
        }
        if (inf.model == 1) continue;
        int loc = inf.out_loc_end;
        size_t n2; int nvec; unsigned done;
        uint32_t *p = spvemu_prerast(w, nw, "main", inf.clip_n ? loc : -1, 1, inf.has_drawindex ? 128 : -1, &n2, &nvec, &done);
        if (!p) { printf("  prerast FALHOU\n"); fails++; continue; }
        printf("  prerast: done=0x%x nvec=%d\n", done, nvec);
        size_t n3; uint32_t *q = clipcull_lower(p, n2, &n3);
        const uint32_t *fin = q ? q : p; size_t fin_n = q ? n3 : n2;
        wr(v[i], "emu", fin, fin_n);
        if (inf.model == 0) {
            size_t ng; uint32_t *g = spvemu_gs_poly(fin, fin_n, "main", 0, 2 | 8, &ng);
            if (!g) { printf("  gs linhas FALHOU\n"); fails++; } else { wr(v[i], "gsl", g, ng); free(g); }
            g = spvemu_gs_poly(fin, fin_n, "main", 1, 0, &ng);
            if (!g) { printf("  gs pontos FALHOU\n"); fails++; } else { wr(v[i], "gsp", g, ng); free(g); }
        }
        free(q);
        free(p); free(w);
    }
    size_t nn; uint32_t *fn = spvemu_fs_new(9, 2, &nn); wr("fsnew", "new", fn, nn); free(fn);
    fn = spvemu_fs_new(9, 1, &nn); wr("fsnew1", "new", fn, nn); free(fn);
    return fails;
}
