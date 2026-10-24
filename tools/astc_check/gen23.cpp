#include "gpu/vulkan/bc_decoder.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <random>
#include <vector>
int main(){
  const int N=65536; std::mt19937 r(11);
  std::vector<uint8_t> bc(N*16), astc(N*16), kind(N);
  for(int b=0;b<N;b++){
    uint8_t* p=&bc[b*16];
    for(int i=0;i<16;i++) p[i]=r();
    int k=b%6; kind[b]=k;
    bool bc3 = k<3;               // kinds 0-2 BC3, 3-5 BC2
    int style=k%3;                // 0 random alpha, 1 smooth alpha, 2 binary alpha
    if(bc3){
      if(style==1){ int a1=r()%200, a0=a1+10+r()%50; p[0]=a0; p[1]=a1; // gradient-ish: indices ordered
        uint64_t bits=0; for(int i=0;i<16;i++){ int idx=2+((i*5)/16+ (r()%2)); if(idx>7) idx=7; bits|=uint64_t(idx)<<(3*i);} for(int i=0;i<6;i++) p[2+i]=bits>>(8*i);} 
      if(style==2){ p[0]=255; p[1]=0; uint64_t bits=0; for(int i=0;i<16;i++) bits|=uint64_t(r()&1)<<(3*i); for(int i=0;i<6;i++) p[2+i]=bits>>(8*i);} 
    } else {
      if(style==1){ int base=r()%160; for(int i=0;i<8;i++){ int a=base+ (i*2)+(r()%3), c=a+2; if(c>15)c=15; if(a>15)a=15; p[i]=(c<<4)|a; } }
      if(style==2){ for(int i=0;i<8;i++){ int a=(r()&1)?15:0, c=(r()&1)?15:0; p[i]=(c<<4)|a; } }
    }
    vk::bc::TranscodeBc23BlockToAstc(p,bc3,&astc[b*16]);
  }
  FILE*f=fopen("bc23/blocks.bc","wb"); fwrite(bc.data(),1,bc.size(),f); fclose(f);
  f=fopen("bc23/blocks.astc","wb"); fwrite(astc.data(),1,astc.size(),f); fclose(f);
  f=fopen("bc23/kinds","wb"); fwrite(kind.data(),1,kind.size(),f); fclose(f);
}
