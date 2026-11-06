/* Immutable replacement images. Workers never read guest RAM or call OpenGL.
 * The cache owns every GL name; guest entries retain indices, never GL names.
 * Thus eviction cannot leave a stale replacement handle in a guest entry. */
#include "gl_texture_pack.h"
#include "pack_image.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUEUE_SIZE 64
enum { IDLE,QUEUED,READING,READY,UPLOADING,LOADED,FAILED };
typedef struct {
    uint64_t hash,last_use,bytes;
    char *path[3]; /* DDS, PNG, BMP, within the highest-priority source root. */
    unsigned category,priority,levels;
    int state;
    GLuint id;
    size_t reserved;
    HitzPackImage image;
} Entry;
static struct {
    Entry *entry;unsigned count,capacity;
    int *map;unsigned mask;
    int configured,enabled,live,category[5],async,log,stop;
    unsigned formats,max_size;
    uint64_t gpu_budget,cpu_budget;
    double upload_ms;
    SDL_mutex *lock;SDL_cond *wake;SDL_Thread *worker;
    unsigned requests[QUEUE_SIZE],ready[QUEUE_SIZE],rqhead,rqtail,rqn,rdhead,rdtail,rdn;
    HitzGlPackStats stats;
    void (*invalidate)(GLuint);
} p;
static void release(unsigned i);
static const char *categories[]={"rink","logos","jerseys","glow","other"};
static uint64_t now_us(void)
{uint64_t c=SDL_GetPerformanceCounter(),f=SDL_GetPerformanceFrequency();return c/f*1000000ull+(c%f)*1000000ull/f;}
static uint64_t megabytes(const char *key,unsigned fallback,unsigned maximum)
{
    const char *s=getenv(key);char *end;unsigned long n=s?strtoul(s,&end,10):fallback;
    if(s&&(!*s||*end))n=fallback;
    if(n<1)n=1;if(n>maximum)n=maximum;
    return (uint64_t)n<<20;
}
static void data_root(char *out,size_t size)
{
    const char *xdg=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
    if(xdg&&*xdg)snprintf(out,size,"%s/nhlhitzpro",xdg);
    else if(home&&*home)snprintf(out,size,"%s/.local/share/nhlhitzpro",home);
    else *out=0;
}
static void settings(void)
{
    for(int i=0;i<5;i++)p.category[i]=1;
    char root[PATH_MAX],path[PATH_MAX],line[512],section[80]="";
    data_root(root,sizeof(root));
    if(*root&&snprintf(path,sizeof(path),"%s/settings.ini",root)<(int)sizeof(path)){
        FILE *f=fopen(path,"r");
        if(f){while(fgets(line,sizeof(line),f)){
            char *s=line;while(isspace((unsigned char)*s))s++;
            if(*s=='['){char *e=strchr(++s,']');if(e){*e=0;snprintf(section,sizeof(section),"%s",s);}continue;}
            if(strcasecmp(section,"textures"))continue;
            char key[80];int value;
            if(sscanf(s,"%79[^=]=%d",key,&value)!=2)continue;
            size_t n=strlen(key);while(n&&isspace((unsigned char)key[n-1]))key[--n]=0;
            if(!strcasecmp(key,"enabled"))p.enabled=value!=0;
            for(int i=0;i<5;i++)if(!strcasecmp(key,categories[i]))p.category[i]=value!=0;
        }fclose(f);}
    }
    const char *v=getenv("RECOMP_HIRES");if(v)p.enabled=atoi(v)!=0;
    for(int i=0;i<5;i++){
        char key[80];snprintf(key,sizeof(key),"RECOMP_PACK_%s",categories[i]);
        for(char *s=key;*s;s++)*s=(char)toupper((unsigned char)*s);
        if((v=getenv(key)))p.category[i]=atoi(v)!=0;
    }
    p.async=!(getenv("RECOMP_PACK_ASYNC")&&atoi(getenv("RECOMP_PACK_ASYNC"))==0);
    p.log=getenv("RECOMP_PACK_LOG")&&atoi(getenv("RECOMP_PACK_LOG"));
    p.gpu_budget=megabytes("RECOMP_PACK_CACHE_MB",128,2048);
    p.cpu_budget=megabytes("RECOMP_PACK_PENDING_MB",64,256);
    p.upload_ms=getenv("RECOMP_PACK_UPLOAD_MS")?atof(getenv("RECOMP_PACK_UPLOAD_MS")):2.0;
    if(!(p.upload_ms>0&&p.upload_ms<=16))p.upload_ms=2.0;
}
static void scan(const char *dir,unsigned depth,unsigned category,unsigned priority)
{
    DIR *d=opendir(dir);if(!d)return;
    struct dirent *ent;
    while((ent=readdir(d))){
        if(ent->d_name[0]=='.')continue;
        char full[PATH_MAX];
        if(snprintf(full,sizeof(full),"%s/%s",dir,ent->d_name)>=(int)sizeof(full))continue;
        struct stat st;if(stat(full,&st))continue;
        if(S_ISDIR(st.st_mode)){
            unsigned cat=category;
            if(!depth){cat=4;for(unsigned i=0;i<5;i++)if(!strcasecmp(ent->d_name,categories[i]))cat=i;}
            if(depth<3)scan(full,depth+1,cat,priority);
            continue;
        }
        if(!S_ISREG(st.st_mode)||strlen(ent->d_name)<20)continue;
        const char *ext=strrchr(ent->d_name,'.');if(!ext)continue;
        int kind=!strcasecmp(ext,".dds")?0:!strcasecmp(ext,".png")?1:!strcasecmp(ext,".bmp")?2:-1;
        if(kind<0)continue;
        char hex[17];memcpy(hex,ent->d_name,16);hex[16]=0;
        int valid=1;for(int i=0;i<16;i++)if(!isxdigit((unsigned char)hex[i]))valid=0;
        if(!valid)continue;
        uint64_t hash=strtoull(hex,NULL,16);unsigned i;
        for(i=0;i<p.count;i++)if(p.entry[i].hash==hash)break;
        if(i==p.count){
            if(p.count>=32768)continue;
            if(p.count==p.capacity){
                unsigned cap=p.capacity?p.capacity*2:256;
                Entry *entries=realloc(p.entry,(size_t)cap*sizeof(*entries));if(!entries)break;
                p.entry=entries;p.capacity=cap;
            }
            memset(&p.entry[i],0,sizeof(Entry));p.entry[i].hash=hash;
            p.entry[i].priority=priority;p.entry[i].category=category;p.count++;
        }
        Entry *e=&p.entry[i];
        if(e->priority!=priority)continue; /* Source priority precedes format preference. */
        if(!e->path[kind])e->path[kind]=strdup(full);
    }
    closedir(d);
}
static void map_build(void)
{
    unsigned size=16;while(size<p.count*2)size<<=1;
    p.map=malloc(size*sizeof(*p.map));if(!p.map)return;
    p.mask=size-1;for(unsigned i=0;i<size;i++)p.map[i]=-1;
    for(unsigned i=0;i<p.count;i++){
        unsigned h=(unsigned)(p.entry[i].hash^(p.entry[i].hash>>29))&p.mask;
        while(p.map[h]>=0)h=(h+1)&p.mask;p.map[h]=(int)i;
    }
}
static int read_entry(unsigned index,HitzPackImage *out,size_t *reserved)
{
    Entry *e=&p.entry[index];
    for(unsigned kind=0;kind<3;kind++){
        if(!e->path[kind])continue;
        size_t bytes=hitz_pack_image_estimate(e->path[kind]);
        if(!bytes||bytes>p.cpu_budget)continue;
        SDL_LockMutex(p.lock);
        if(!p.async&&p.stats.pending_bytes+bytes>p.cpu_budget){SDL_UnlockMutex(p.lock);return -1;}
        while(!p.stop&&p.stats.pending_bytes+bytes>p.cpu_budget)SDL_CondWait(p.wake,p.lock);
        if(p.stop){SDL_UnlockMutex(p.lock);return 0;}
        p.stats.pending_bytes+=bytes;SDL_UnlockMutex(p.lock);
        uint64_t began=now_us();
        int ok=hitz_pack_image_read(e->path[kind],bytes,out);
        ok=ok&&out->width<=p.max_size&&out->height<=p.max_size&&(p.formats&(1u<<out->format));
        SDL_LockMutex(p.lock);p.stats.load_us+=now_us()-began;
        if(!ok){hitz_pack_image_free(out);p.stats.pending_bytes-=bytes;SDL_CondBroadcast(p.wake);}
        SDL_UnlockMutex(p.lock);
        if(ok){*reserved=bytes;return 1;}
    }
    return 0;
}
static int worker(void *unused)
{
    (void)unused;SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for(;;){
        SDL_LockMutex(p.lock);
        while(!p.stop&&!p.rqn)SDL_CondWait(p.wake,p.lock);
        if(p.stop){SDL_UnlockMutex(p.lock);break;}
        unsigned i=p.requests[p.rqhead];p.rqhead=(p.rqhead+1)%QUEUE_SIZE;p.rqn--;
        p.entry[i].state=READING;SDL_UnlockMutex(p.lock);
        HitzPackImage image={0};size_t reserved=0;int ok=read_entry(i,&image,&reserved);
        SDL_LockMutex(p.lock);
        while(!p.stop&&ok&&p.rdn==QUEUE_SIZE)SDL_CondWait(p.wake,p.lock);
        if(p.stop){hitz_pack_image_free(&image);p.stats.pending_bytes-=reserved;SDL_UnlockMutex(p.lock);break;}
        p.stats.loads++;
        if(ok){p.entry[i].image=image;p.entry[i].reserved=reserved;p.entry[i].state=READY;
            p.ready[p.rdtail]=i;p.rdtail=(p.rdtail+1)%QUEUE_SIZE;p.rdn++;}
        else {p.entry[i].state=FAILED;p.stats.failures++;}
        SDL_UnlockMutex(p.lock);
    }
    return 0;
}
void hitz_gl_pack_init(unsigned scale,void (*invalidate)(GLuint))
{
    if(p.configured)return;
    p.configured=1;p.invalidate=invalidate;settings();
    /* JOJO item 26: index the pack even when off (as backend_d3d11.c), so F1 can switch it live */
    p.live=p.enabled;(void)scale;   /* item 27: scanned at every scale; Render Scale changes live */
    p.formats=(1u<<HITZ_PACK_BGRA8)|(1u<<HITZ_PACK_RGBA8);
    if(epoxy_gl_version()>=42||epoxy_has_gl_extension("GL_ARB_texture_compression_bptc"))p.formats|=1u<<HITZ_PACK_BC7;
    if(epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc"))p.formats|=(1u<<HITZ_PACK_BC1)|(1u<<HITZ_PACK_BC2)|(1u<<HITZ_PACK_BC3);
    else if(epoxy_has_gl_extension("GL_EXT_texture_compression_dxt1"))p.formats|=1u<<HITZ_PACK_BC1;
    GLint max_size;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_size);p.max_size=(unsigned)max_size;
    char root[PATH_MAX],path[PATH_MAX];const char *explicit_path=getenv("RECOMP_TEXPACK");
    if(explicit_path&&*explicit_path)scan(explicit_path,0,4,0);
    else {
        data_root(root,sizeof(root));
        if(*root&&snprintf(path,sizeof(path),"%s/texture_pack",root)<(int)sizeof(path))scan(path,0,4,0);
        ssize_t n=readlink("/proc/self/exe",root,sizeof(root)-1);
        if(n>0){root[n]=0;char *slash=strrchr(root,'/');if(slash){*slash=0;if(snprintf(path,sizeof(path),"%s/texture_pack",root)<(int)sizeof(path))scan(path,0,4,1);}}
    }
    map_build();
    if(!p.count||!p.map)return;
    p.lock=SDL_CreateMutex();p.wake=SDL_CreateCond();
    if(!p.lock||!p.wake){p.enabled=0;return;}
    if(p.async){p.worker=SDL_CreateThread(worker,"texture-pack",NULL);if(!p.worker)p.async=0;}
    p.stats.images=p.count;
    fprintf(stderr,"[GL-PACK] %u images, cache=%llu MiB, pending=%llu MiB, async=%d, BC7=%d\n",p.count,(unsigned long long)(p.gpu_budget>>20),(unsigned long long)(p.cpu_budget>>20),p.async,!!(p.formats&(1u<<HITZ_PACK_BC7)));
}
int hitz_gl_pack_active(unsigned scale)
{return p.live&&scale>=2&&p.count&&p.map&&p.lock&&p.wake;}
int hitz_gl_pack_available(unsigned scale)
{return scale>=2&&p.count&&p.map&&p.lock&&p.wake;}
void hitz_gl_pack_set_live(int on)
{   /* off: free every resident replacement; queued reads finish and are dropped on upload */
    p.live=on!=0;p.enabled=p.live;
    if(!p.live&&p.lock)for(unsigned i=0;i<p.count;i++)release(i);
    fprintf(stderr,"[GL-PACK] live %s\n",p.live?"on":"off");
}
int hitz_gl_pack_find(uint64_t hash)
{
    if(!hash||!p.map)return -1;
    unsigned h=(unsigned)(hash^(hash>>29))&p.mask;
    while(p.map[h]>=0){Entry *e=&p.entry[p.map[h]];if(e->hash==hash)return p.category[e->category]?p.map[h]:-1;h=(h+1)&p.mask;}
    return -1;
}
static size_t gpu_bytes(const HitzPackImage *im,unsigned *levels)
{
    *levels=im->mips;
    if(!im->generate_mips)return im->payload_bytes;
    size_t total=0;uint32_t w=im->width,h=im->height;*levels=0;
    do{total+=(size_t)w*h*4;(*levels)++;if(w==1&&h==1)break;w=w>1?w/2:1;h=h>1?h/2:1;}while(1);
    return total;
}
static void release(unsigned i)
{
    Entry *e=&p.entry[i];GLuint id=e->id;
    if(!id)return;
    glDeleteTextures(1,&id);if(p.invalidate)p.invalidate(id);
    p.stats.resident_bytes-=e->bytes;e->id=0;e->bytes=0;e->levels=0;
    SDL_LockMutex(p.lock);e->state=IDLE;p.stats.evictions++;SDL_UnlockMutex(p.lock);
}
static int room(size_t needed,uint64_t frame)
{
    if(needed>p.gpu_budget)return -1;
    while(p.stats.resident_bytes+needed>p.gpu_budget){
        unsigned victim=p.count;
        for(unsigned i=0;i<p.count;i++)if(p.entry[i].id&&p.entry[i].last_use!=frame&&(victim==p.count||p.entry[i].last_use<p.entry[victim].last_use))victim=i;
        if(victim==p.count)return 0;
        release(victim);
    }
    return 1;
}
static GLuint upload(const HitzPackImage *im,unsigned levels)
{
    GLuint id;glGenTextures(1,&id);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,id);
    GLenum internal=GL_RGBA8;
    switch(im->format){
    case HITZ_PACK_BC1:internal=GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;break;
    case HITZ_PACK_BC2:internal=GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;break;
    case HITZ_PACK_BC3:internal=GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;break;
    case HITZ_PACK_BC7:internal=GL_COMPRESSED_RGBA_BPTC_UNORM;break;
    default:break;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    for(unsigned i=0;i<im->mips;i++){
        const HitzPackLevel *m=&im->level[i];
        if(internal!=GL_RGBA8)glCompressedTexImage2D(GL_TEXTURE_2D,i,internal,m->width,m->height,0,(GLsizei)m->bytes,m->data);
        else glTexImage2D(GL_TEXTURE_2D,i,GL_RGBA8,m->width,m->height,0,im->format==HITZ_PACK_BGRA8?GL_BGRA:GL_RGBA,GL_UNSIGNED_BYTE,m->data);
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,levels-1);
    if(im->opaque)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_ONE);
    if(im->generate_mips&&levels>1)glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,levels>1?GL_LINEAR_MIPMAP_LINEAR:GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    GLenum err=glGetError();
    if(p.invalidate)p.invalidate(0);
    if(err!=GL_NO_ERROR){fprintf(stderr,"[GL-PACK] upload GL error 0x%x (%ux%u format %d)\n",err,im->width,im->height,im->format);glDeleteTextures(1,&id);return 0;}
    return id;
}
void hitz_gl_pack_pump(uint64_t frame)
{
    if(!p.lock)return;
    uint64_t start=now_us();unsigned made=0;
    while(made<16){
        SDL_LockMutex(p.lock);
        if(!p.rdn){SDL_UnlockMutex(p.lock);break;}
        unsigned i=p.ready[p.rdhead];SDL_UnlockMutex(p.lock);
        Entry *e=&p.entry[i];unsigned levels;size_t bytes=gpu_bytes(&e->image,&levels);
        int available=room(bytes,frame);
        if(!available)break; /* Leave it ready until a later frame can admit it. */
        SDL_LockMutex(p.lock);p.rdhead=(p.rdhead+1)%QUEUE_SIZE;p.rdn--;e->state=UPLOADING;SDL_CondBroadcast(p.wake);SDL_UnlockMutex(p.lock);
        uint64_t began=now_us();GLuint id=available>0?upload(&e->image,levels):0;
        p.stats.create_us+=now_us()-began;
        if(id){e->id=id;e->levels=levels;e->bytes=bytes;e->last_use=frame;p.stats.resident_bytes+=bytes;}
        hitz_pack_image_free(&e->image);
        SDL_LockMutex(p.lock);p.stats.pending_bytes-=e->reserved;e->reserved=0;e->state=id?LOADED:FAILED;
        if(!id)p.stats.failures++;
        SDL_CondBroadcast(p.wake);SDL_UnlockMutex(p.lock);
        if(p.log)fprintf(stderr,"[GL-PACK] %s #%u %016llX (%zu GPU bytes)\n",id?"resident":"fallback",i,(unsigned long long)e->hash,bytes);
        made++;if(now_us()-start>=(uint64_t)(p.upload_ms*1000))break;
    }
}
GLuint hitz_gl_pack_get(int index,uint64_t frame,unsigned *levels)
{
    if(levels)*levels=1;
    if(index<0||(unsigned)index>=p.count||!p.lock)return 0;
    Entry *e=&p.entry[index];
    if(e->id){e->last_use=frame;p.stats.hits++;if(levels)*levels=e->levels;return e->id;}
    SDL_LockMutex(p.lock);
    if(e->state!=IDLE){SDL_UnlockMutex(p.lock);return 0;}
    if(p.async){
        if(p.rqn<QUEUE_SIZE){e->state=QUEUED;p.requests[p.rqtail]=(unsigned)index;p.rqtail=(p.rqtail+1)%QUEUE_SIZE;p.rqn++;SDL_CondBroadcast(p.wake);}
        SDL_UnlockMutex(p.lock);return 0;
    }
    if(p.rdn==QUEUE_SIZE){SDL_UnlockMutex(p.lock);return 0;}
    e->state=READING;SDL_UnlockMutex(p.lock);
    HitzPackImage image={0};size_t reserved=0;int ok=read_entry((unsigned)index,&image,&reserved);
    SDL_LockMutex(p.lock);p.stats.loads++;
    if(ok>0){e->image=image;e->reserved=reserved;e->state=READY;p.ready[p.rdtail]=(unsigned)index;p.rdtail=(p.rdtail+1)%QUEUE_SIZE;p.rdn++;}
    else if(ok<0)e->state=IDLE;
    else{e->state=FAILED;p.stats.failures++;}
    SDL_UnlockMutex(p.lock);
    hitz_gl_pack_pump(frame);
    if(e->id){e->last_use=frame;if(levels)*levels=e->levels;return e->id;}
    return 0;
}
void hitz_gl_pack_stats(HitzGlPackStats *stats)
{
    if(p.lock)SDL_LockMutex(p.lock);
    *stats=p.stats;stats->queued=p.rqn+p.rdn;
    if(p.lock)SDL_UnlockMutex(p.lock);
}
void hitz_gl_pack_shutdown(void)
{
    if(p.lock){SDL_LockMutex(p.lock);p.stop=1;if(p.wake)SDL_CondBroadcast(p.wake);SDL_UnlockMutex(p.lock);}
    if(p.worker)SDL_WaitThread(p.worker,NULL);
    for(unsigned i=0;i<p.count;i++){
        if(p.entry[i].id){glDeleteTextures(1,&p.entry[i].id);if(p.invalidate)p.invalidate(p.entry[i].id);}
        hitz_pack_image_free(&p.entry[i].image);
        for(unsigned k=0;k<3;k++)free(p.entry[i].path[k]);
    }
    free(p.entry);free(p.map);if(p.wake)SDL_DestroyCond(p.wake);if(p.lock)SDL_DestroyMutex(p.lock);
    memset(&p,0,sizeof(p));
}
