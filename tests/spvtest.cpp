// spvtest: roda os passes Mali (spvfix) em arquivos .spv, mede tempo e grava <arq>.fix.spv
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <ctime>
#include "../shim/spvfix.h"
static double now(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
int main(int c, char **v){
  unsigned mask = 0x3f; int first = 1;
  if (c > 2 && v[1][0]=='-' && v[1][1]=='m') { mask = strtoul(v[2],0,0); first = 3; }
  double tot = 0; int changed = 0, err = 0, n = 0;
  for (int i = first; i < c; i++) {
    FILE *f = fopen(v[i], "rb"); if (!f) continue;
    std::vector<uint32_t> b; uint32_t w; while (fread(&w,4,1,f)==1) b.push_back(w); fclose(f);
    uint32_t *o = 0; size_t on = 0; double t0 = now();
    int r = spvfix_run(b.data(), b.size(), mask, &o, &on); double dt = now()-t0; tot += dt; n++;
    if (r == 0) { changed++; char p[512]; snprintf(p,sizeof p,"%s.fix.spv",v[i]); FILE *g=fopen(p,"wb"); fwrite(o,4,on,g); fclose(g); free(o); }
    if (r < 0) err++;
    printf("%-60s r=%d %zu->%zu words %.2f ms\n", v[i], r, b.size(), r==0?on:b.size(), dt*1e3);
  }
  printf("TOTAL %d arquivos, %d alterados, %d erros, %.1f ms (media %.2f ms)\n", n, changed, err, tot*1e3, n?tot*1e3/n:0);
}
