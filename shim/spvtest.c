#include "shim.c"
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); fseek(f, 0, 2); long n = ftell(f); rewind(f);
    uint32_t *b = malloc(n); fread(b, 1, n, f); fclose(f);
    size_t nw; uint32_t *o = spv_patch(b, n / 4, &nw);
    if (!o) { printf("sem patch\n"); return 2; }
    f = fopen(v[2], "wb"); fwrite(o, 4, nw, f); fclose(f); return 0;
}
