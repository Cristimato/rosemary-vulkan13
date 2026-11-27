#include <stdio.h>
#include <stdlib.h>
#include "../shim/spvemu.h"
int main(int c, char **v) {
    for (int i = 1; i < c; i++) {
        FILE *f = fopen(v[i], "rb"); fseek(f, 0, 2); long l = ftell(f); fseek(f, 0, 0);
        uint32_t *w = malloc(l); if (fread(w, 1, l, f) != (size_t)l) return 1; fclose(f);
        size_t n; int cnt; uint32_t *o = spvemu_fp16(w, l / 4, &n, &cnt);
        printf("%-28s fp16: %d decorações\n", v[i], cnt);
        if (!o) continue;
        char p[600]; snprintf(p, sizeof p, "%s.fp16.spv", v[i]); f = fopen(p, "wb"); fwrite(o, 4, n, f); fclose(f);
    }
    return 0;
}
