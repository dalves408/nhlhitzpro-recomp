/* Replacement file data only: no guest-memory access and no graphics calls. */
#include "pack_image.h"
#include <SDL2/SDL.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t le32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint32_t be32(const unsigned char *p)
{ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|(uint32_t)p[3]; }
static int extension(const char *p,const char *e)
{ const char *dot=strrchr(p,'.');return dot&&!strcasecmp(dot,e); }
static int dimensions(uint32_t w,uint32_t h)
{ return w&&h&&w<=16384&&h<=16384; }

size_t hitz_pack_level_bytes(HitzPackFormat format,uint32_t w,uint32_t h)
{
    if(!dimensions(w,h))return 0;
    switch(format){
    case HITZ_PACK_BC1:return (size_t)((w+3)/4)*((h+3)/4)*8;
    case HITZ_PACK_BC2:case HITZ_PACK_BC3:case HITZ_PACK_BC7:
        return (size_t)((w+3)/4)*((h+3)/4)*16;
    case HITZ_PACK_BGRA8:case HITZ_PACK_RGBA8:return (size_t)w*h*4;
    default:return 0;
    }
}

int hitz_pack_parse_dds(const void *bytes,size_t length,HitzPackImage *im)
{
    const unsigned char *b=bytes;
    size_t offset=128;
    memset(im,0,sizeof(*im));
    if(!b||length<128||memcmp(b,"DDS ",4)||le32(b+4)!=124||le32(b+76)!=32)return 0;
    uint32_t w=le32(b+16),h=le32(b+12),mips=le32(b+28);
    uint32_t caps2=le32(b+112),four=le32(b+84),flags=le32(b+80);
    if(!dimensions(w,h)||le32(b+24)>1||(caps2&(0xfe00u|0x200000u)))return 0;
    unsigned max_mips=1;
    for(uint32_t n=w>h?w:h;n>1;n>>=1)max_mips++;
    if(!mips)mips=1;
    if(mips>max_mips||mips>HITZ_PACK_MAX_MIPS)return 0;
    HitzPackFormat format=HITZ_PACK_INVALID;
    if((flags&4)&&four==0x30315844u){ /* DX10 */
        if(length<148||le32(b+132)!=3||le32(b+140)!=1||(le32(b+136)&4))return 0;
        switch(le32(b+128)){
        case 71:format=HITZ_PACK_BC1;break;
        case 74:format=HITZ_PACK_BC2;break;
        case 77:format=HITZ_PACK_BC3;break;
        case 98:format=HITZ_PACK_BC7;break;
        case 87:format=HITZ_PACK_BGRA8;break;
        case 28:format=HITZ_PACK_RGBA8;break;
        default:return 0; /* Do not silently reinterpret sRGB or other formats. */
        }
        offset=148;
    }else if(flags&4){
        if(four==0x31545844u)format=HITZ_PACK_BC1;
        else if(four==0x33545844u)format=HITZ_PACK_BC2;
        else if(four==0x35545844u)format=HITZ_PACK_BC3;
        else return 0;
    }else if((flags&0x40)&&le32(b+88)==32&&le32(b+96)==0x0000ff00){
        uint32_t r=le32(b+92),blue=le32(b+100),alpha=le32(b+104);
        if(r==0x00ff0000&&blue==0x000000ff)format=HITZ_PACK_BGRA8;
        else if(r==0x000000ff&&blue==0x00ff0000)format=HITZ_PACK_RGBA8;
        else return 0;
        if(alpha!=0&&alpha!=0xff000000u)return 0;
        im->opaque=!(flags&1)||!alpha;
    }else return 0;
    /* Only tightly packed 32-bit rows are supported by the upload path. */
    if((format==HITZ_PACK_BGRA8||format==HITZ_PACK_RGBA8)&&
       (le32(b+8)&8)&&le32(b+20)!=w*4)return 0;
    im->width=w;im->height=h;im->mips=mips;im->format=format;
    for(unsigned i=0;i<mips;i++){
        size_t n=hitz_pack_level_bytes(format,w,h);
        if(!n||offset>length||n>length-offset){memset(im,0,sizeof(*im));return 0;}
        im->level[i]=(HitzPackLevel){b+offset,n,w,h};
        im->payload_bytes+=n;offset+=n;
        w=w>1?w/2:1;h=h>1?h/2:1;
    }
    im->memory_bytes=length;
    return 1;
}

size_t hitz_pack_image_estimate(const char *path)
{
    struct stat s;
    if(stat(path,&s)||s.st_size<=0)return 0;
    if(extension(path,".dds"))return s.st_size>=128?(size_t)s.st_size:0;
    FILE *f=fopen(path,"rb");unsigned char b[32];
    if(!f)return 0;
    size_t n=fread(b,1,sizeof(b),f);fclose(f);
    if(extension(path,".png")){
        if(n<24||png_sig_cmp(b,0,8)||memcmp(b+12,"IHDR",4)||be32(b+8)!=13)return 0;
        uint32_t w=be32(b+16),h=be32(b+20);
        return dimensions(w,h)?(size_t)w*h*4:0;
    }
    if(extension(path,".bmp")){
        if(n<26||b[0]!='B'||b[1]!='M'||le32(b+14)<40)return 0;
        uint32_t w=le32(b+18);int32_t signed_h=(int32_t)le32(b+22);
        if(signed_h==INT32_MIN)return 0;
        uint32_t h=(uint32_t)(signed_h<0?-signed_h:signed_h);
        return dimensions(w,h)?(size_t)w*h*8:0; /* Source and converted surfaces. */
    }
    return 0;
}

int hitz_pack_image_read(const char *path,size_t limit,HitzPackImage *im)
{
    memset(im,0,sizeof(*im));
    size_t estimated=hitz_pack_image_estimate(path);
    if(!estimated||estimated>limit)return 0;
    if(extension(path,".dds")){
        FILE *f=fopen(path,"rb");
        if(!f)return 0;
        unsigned char *b=malloc(estimated);
        int read=b&&fread(b,1,estimated,f)==estimated&&fgetc(f)==EOF;
        fclose(f);
        if(!read||!hitz_pack_parse_dds(b,estimated,im)){free(b);return 0;}
        im->allocation=b;return 1;
    }
    if(extension(path,".png")){
        png_image p={0};p.version=PNG_IMAGE_VERSION;
        if(!png_image_begin_read_from_file(&p,path))return 0;
        size_t size=(size_t)p.width*p.height*4;
        if(!dimensions(p.width,p.height)||size>limit){png_image_free(&p);return 0;}
        p.format=PNG_FORMAT_BGRA;
        unsigned char *b=malloc(size);
        if(!b||!png_image_finish_read(&p,NULL,b,0,NULL)){free(b);png_image_free(&p);return 0;}
        im->width=p.width;im->height=p.height;im->format=HITZ_PACK_BGRA8;
        im->allocation=b;im->memory_bytes=im->payload_bytes=size;im->mips=1;im->generate_mips=1;
        im->level[0]=(HitzPackLevel){b,size,p.width,p.height};png_image_free(&p);return 1;
    }
    SDL_Surface *src=SDL_LoadBMP(path);
    if(!src)return 0;
    SDL_Surface *converted=SDL_ConvertSurfaceFormat(src,SDL_PIXELFORMAT_ARGB8888,0);
    SDL_FreeSurface(src);
    if(!converted)return 0;
    if(!dimensions((uint32_t)converted->w,(uint32_t)converted->h)){SDL_FreeSurface(converted);return 0;}
    size_t size=(size_t)converted->w*converted->h*4;
    unsigned char *b=size<=limit?malloc(size):NULL;
    if(b)for(int y=0;y<converted->h;y++)memcpy(b+(size_t)y*converted->w*4,(char*)converted->pixels+(size_t)y*converted->pitch,(size_t)converted->w*4);
    im->width=converted->w;im->height=converted->h;SDL_FreeSurface(converted);
    if(!b){memset(im,0,sizeof(*im));return 0;}
    im->allocation=b;im->memory_bytes=im->payload_bytes=size;im->mips=1;im->generate_mips=1;im->format=HITZ_PACK_BGRA8;
    im->level[0]=(HitzPackLevel){b,size,im->width,im->height};return 1;
}

void hitz_pack_image_free(HitzPackImage *im)
{ free(im->allocation);memset(im,0,sizeof(*im)); }
