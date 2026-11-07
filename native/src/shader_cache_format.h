#ifndef HITZ_SHADER_CACHE_FORMAT_H
#define HITZ_SHADER_CACHE_FORMAT_H
/* Shared by the runtime and offline preparer. Keep compiler identity and exact
 * source bytes in the key; a shader from another revision is never substituted. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3dcompiler.h>
#define SC_MAGIC 0x31434853u
#define SC_FORMAT 3u
#define SC_SOURCE_MAGIC 0x31535348u
typedef struct { uint32_t magic, format; uint64_t h1, h2; uint32_t srclen, size; uint64_t sum; } ScHdr;
typedef struct { uint32_t magic, format, srclen; char entry[32], target[16]; } ScSourceHdr;

static uint64_t sc_hash(const void *p, size_t n, uint64_t h)
{
    const unsigned char *s=(const unsigned char *)p; size_t i;
    for(i=0;i<n;i++){h^=s[i];h*=1099511628211ull;} return h;
}
static void sc_hash_key(const char *src,size_t len,const char *entry,const char *target,uint64_t *h1,uint64_t *h2)
{
    char tag[96];
    int n=snprintf(tag,sizeof tag,"|%s|%s|opt3|d3dcompiler%d|f%u",entry,target,(int)D3D_COMPILER_VERSION,SC_FORMAT);
    *h1=sc_hash(tag,(size_t)n,sc_hash(src,len,14695981039346656037ull));
    *h2=sc_hash(src,len,sc_hash(tag,(size_t)n,0x9E3779B97F4A7C15ull));
}
static ID3DBlob *sc_load(const char *path,uint64_t h1,uint64_t h2,size_t srclen)
{
    FILE *f=fopen(path,"rb");ScHdr h;ID3DBlob *blob=NULL;
    if(!f)return NULL;
    if(fread(&h,sizeof h,1,f)==1 && h.magic==SC_MAGIC && h.format==SC_FORMAT && h.h1==h1 && h.h2==h2
       && h.srclen==(uint32_t)srclen && h.size && h.size<(64u<<20)
       && SUCCEEDED(D3DCreateBlob(h.size,&blob))) {
        void *p=ID3D10Blob_GetBufferPointer(blob);
        if(fread(p,1,h.size,f)!=h.size || fgetc(f)!=EOF || sc_hash(p,h.size,14695981039346656037ull)!=h.sum) {
            ID3D10Blob_Release(blob);blob=NULL;
        }
    }
    fclose(f);return blob;
}
static int sc_write(const char *path,uint64_t h1,uint64_t h2,size_t srclen,ID3DBlob *blob)
{
    static volatile LONG seq;char tmp[MAX_PATH+96];ScHdr h;FILE *f;int wrote,closed;
    size_t n=ID3D10Blob_GetBufferSize(blob);const void *p=ID3D10Blob_GetBufferPointer(blob);
    snprintf(tmp,sizeof tmp,"%s.%lu.%ld.tmp",path,GetCurrentThreadId(),InterlockedIncrement(&seq));
    f=fopen(tmp,"wb");if(!f)return 0;
    h.magic=SC_MAGIC;h.format=SC_FORMAT;h.h1=h1;h.h2=h2;h.srclen=(uint32_t)srclen;h.size=(uint32_t)n;
    h.sum=sc_hash(p,n,14695981039346656037ull);
    wrote=fwrite(&h,sizeof h,1,f)==1 && fwrite(p,1,n,f)==n;closed=fclose(f)==0;
    if(wrote && closed && MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING))return 1;
    DeleteFileA(tmp);return 0;
}
#endif
