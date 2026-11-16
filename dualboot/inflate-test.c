#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinf/tinf.h"
int main(int argc,char **argv) {
 if(argc!=4)return 1;
 unsigned int expected=strtoul(argv[3],0,0),n;
 FILE*f=fopen(argv[1],"rb");if(!f)return 2;
 fseek(f,0,SEEK_END);n=ftell(f);rewind(f);
 unsigned char*src=malloc(n),*mem=malloc(expected+32);if(!src||!mem)return 3;
 if(fread(src,1,n,f)!=n)return 4;fclose(f);memset(mem,0xa5,expected+32);
 unsigned int length=expected;
 int result=tinf_gzip_uncompress(mem+16,&length,src,n);
 if(result || length!=expected)return 5;
 for(int i=0;i<16;i++)if(mem[i]!=0xa5||mem[expected+16+i]!=0xa5)return 6;
 f=fopen(argv[2],"wb");if(!f)return 7;if(fwrite(mem+16,1,length,f)!=length)return 8;fclose(f);
 length=expected;result=tinf_gzip_uncompress(mem+16,&length,src,n-8);if(result==0)return 9;
 src[n-8]^=1;length=expected;result=tinf_gzip_uncompress(mem+16,&length,src,n);if(result==0)return 10;
 for(int i=0;i<16;i++)if(mem[i]!=0xa5||mem[expected+16+i]!=0xa5)return 11;
 printf("GZIP_ROUNDTRIP_AND_REJECTION_PASS %s length=%u\n",argv[1],expected);return 0;
}
