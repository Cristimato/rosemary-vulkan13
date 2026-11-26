#include <stdio.h>
#include <stdlib.h>
#include "../shim/clipcull.h"
int main(int c,char**v){ for(int i=1;i<c;i++){ FILE*f=fopen(v[i],"rb"); fseek(f,0,2); long n=ftell(f); fseek(f,0,0); uint32_t*b=malloc(n); fread(b,1,n,f); fclose(f);
  size_t on=0; uint32_t*o=clipcull_lower(b,n/4,&on); printf("%s: %s %ld->%zu words\n",v[i],o?"ALTERADO":"inalterado",n/4,o?on:(size_t)n/4);
  if(o){ char p[512]; snprintf(p,sizeof p,"%s.cc.spv",v[i]); FILE*g=fopen(p,"wb"); fwrite(o,4,on,g); fclose(g);} } }
