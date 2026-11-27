#include <stdio.h>
#include <stdlib.h>
#include "../shim/spvemu.h"
int main(int c, char **v) {
    int fails = 0;
    for (int i = 1; i < c; i++) {
        FILE *f = fopen(v[i], "rb"); fseek(f, 0, 2); long l = ftell(f); fseek(f, 0, 0);
        uint32_t *w = malloc(l); if (fread(w, 1, l, f) != (size_t)l) return 1; fclose(f);
        size_t n; int cnt;
        uint32_t *o = spvemu_waterfall(w, l / 4, 16, &n, &cnt);
        printf("%-28s waterfall: %d operações\n", v[i], cnt);
        if (!o) { if (spvemu_waterfall_needed(w, l / 4)) { printf("  FALHOU (tinha NonUniform)\n"); fails++; } continue; }
        char p[600]; snprintf(p, sizeof p, "%s.wf.spv", v[i]);
        f = fopen(p, "wb"); fwrite(o, 4, n, f); fclose(f);
    }
    return fails;
}
