#include "gpu/vulkan/bc_decoder.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <random>
#include <vector>
int main(){
  const int N=65536; std::mt19937 r(7);
  std::vector<uint8_t> bc(N*8), astc(N*16);
  for(int b=0;b<N;b++){
    uint8_t* p=&bc[b*8];
    for(int i=0;i<8;i++) p[i]=r();
    int kind=b&3;           // 0,1 random; 2 force 3-colour; 3 force 4-colour
    uint16_t c0=p[0]|p[1]<<8, c1=p[2]|p[3]<<8;
    if(kind==2 && c0>c1){ std::swap(c0,c1); }
    if(kind==3 && c0<=c1){ std::swap(c0,c1); if(c0==c1) c0++; }
    p[0]=c0;p[1]=c0>>8;p[2]=c1;p[3]=c1>>8;
    if(kind==2 && (b&4)) { // few transparent texels
      for(int i=4;i<8;i++) p[i]&=0x77; // clears top bit of each nibble pair -> indices 0/1
      p[4]&=~0; }
    vk::bc::TranscodeBc1BlockToAstc(p,&astc[b*16]);
  }
  FILE*f=fopen("blocks.bc1","wb"); fwrite(bc.data(),1,bc.size(),f); fclose(f);
  f=fopen("blocks.astc","wb"); fwrite(astc.data(),1,astc.size(),f); fclose(f);
}
