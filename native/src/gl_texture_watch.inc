/* JOJO item 1: exact "unchanged since verified" test for guest textures using
 * the kernel's soft-dirty page bits (RECOMP_GL_TEXTURE_WATCH=1, off by default).
 *
 * Every flip writes "4" to /proc/self/clear_refs, which clears the soft-dirty
 * bit of every page in the process and write-protects them; the first write to
 * a page afterwards (guest store, kernel copy_to_user, any thread) faults and
 * sets the bit again. A texture verified (memcmp/hash) after the last clear and
 * whose pages are still clean in EVERY mapping of the same memory has not been
 * written since it was verified, so the byte comparison can be skipped. Guest
 * RAM is a memfd mapped many times (mirrors, contiguous/tiled windows); soft-
 * dirty is per mapping, so all writable aliases of the file are checked. Any
 * doubt (dirty page, unknown mapping, too many aliases, read error) falls back
 * to the original exact comparison. */
#include <fcntl.h>
#include <unistd.h>
typedef struct { uintptr_t start,end; uint64_t off; unsigned long dev,ino; int shared,writable; } WatchMap;
static int watch_on=-1,watch_pagemap=-1,watch_clear=-1;
static uint64_t watch_epoch;           /* 0 = never cleared; textures store the epoch they were verified in */
static WatchMap *watch_maps;static unsigned watch_nmaps;
static unsigned watch_max_aliases;
static uint64_t watch_skips,watch_skip_bytes,watch_fallbacks;
static unsigned watch_maps_gen;static int watch_fast=-1;static uint64_t watch_miss_flip=~0ull,watch_flip_no;
static void watch_load_maps(void)
{
    FILE *f=fopen("/proc/self/maps","r");watch_nmaps=0;if(!f)return;
    static unsigned cap;char line[512];
    while(fgets(line,sizeof(line),f)){
        unsigned long s,e,maj,min,ino;unsigned long long off;char perm[8];
        if(sscanf(line,"%lx-%lx %7s %llx %lx:%lx %lu",&s,&e,perm,&off,&maj,&min,&ino)!=7)continue;
        if(watch_nmaps==cap){cap=cap?cap*2:512;WatchMap *m=realloc(watch_maps,cap*sizeof(*m));if(!m){watch_nmaps=0;break;}watch_maps=m;}
        WatchMap *m=&watch_maps[watch_nmaps++];
        m->start=s;m->end=e;m->off=off;m->dev=(maj<<20)|min;m->ino=ino;m->shared=perm[3]=='s';m->writable=perm[1]=='w';
    }
    fclose(f);watch_maps_gen++;
}
static void watch_init(void)
{
    const char *v=getenv("RECOMP_GL_TEXTURE_WATCH");watch_on=v&&v[0]=='1';
    {const char *f=getenv("RECOMP_GL_TEXTURE_WATCH_FAST");watch_fast=f&&f[0]=='1';}
    if(!watch_on)return;
    watch_pagemap=open("/proc/self/pagemap",O_RDONLY|O_CLOEXEC);
    watch_clear=open("/proc/self/clear_refs",O_WRONLY|O_CLOEXEC);
    if(watch_pagemap<0||watch_clear<0){fprintf(stderr,"[GL] texture watch unavailable (pagemap/clear_refs), using full checks\n");watch_on=0;return;}
    fprintf(stderr,"[GL] texture watch: soft-dirty page tracking enabled\n");
}
/* Called at flip: start a new epoch. */
static void watch_flip(void)
{
    watch_flip_no++;
    if(watch_on<0)watch_init();
    if(!watch_on)return;
    static unsigned every,count;
    if(!every){const char *v=getenv("RECOMP_GL_TEXTURE_WATCH_CLEAR_EVERY");every=v&&atoi(v)>0?(unsigned)atoi(v):1;}
    /* Guest RAM aliases are created at start-up; reload the table only every
     * 600 flips or when a lookup misses (watch_clean). */
    static unsigned reload;if(!watch_nmaps||++reload>=600){reload=0;watch_load_maps();}
    if(watch_epoch&&++count<every)return;   /* epoch continues: pages stay dirty until the next clear */
    count=0;
    if(pwrite(watch_clear,"4",1,0)!=1){watch_on=0;fprintf(stderr,"[GL] texture watch: clear_refs failed, disabled\n");return;}
    watch_epoch++;
}
static int watch_range_clean(uintptr_t va,size_t len)
{
    uint64_t ent[64];
    uintptr_t first=va>>12,last=(va+len-1)>>12;
    for(uintptr_t p=first;p<=last;){
        size_t n=last-p+1;if(n>64)n=64;
        if(pread(watch_pagemap,ent,n*8,(off_t)(p*8))!=(ssize_t)(n*8))return 0;
        for(size_t i=0;i<n;i++)if(ent[i]&(1ull<<55))return 0;
        p+=n;
    }
    return 1;
}
/* 1 only if no byte of [va,va+len) can have been written since the current
 * epoch began, through any mapping of the same memory. */
static int watch_clean(const void *ptr,size_t len)
{
    if(!watch_on||!watch_epoch||!len)return 0;
    uintptr_t va=(uintptr_t)ptr;const WatchMap *home=NULL;
    for(unsigned i=0;i<watch_nmaps;i++)if(va>=watch_maps[i].start&&va<watch_maps[i].end){home=&watch_maps[i];break;}
    if(!home&&watch_fast>0&&watch_miss_flip==watch_flip_no)return 0;   /* item 10: at most one miss reload per flip */
    if(!home){watch_miss_flip=watch_flip_no;watch_load_maps();for(unsigned i=0;i<watch_nmaps;i++)if(va>=watch_maps[i].start&&va<watch_maps[i].end){home=&watch_maps[i];break;}}
    if(!home||va+len>home->end)return 0;
    if(!home->shared||!home->ino)return watch_range_clean(va,len);   /* private memory has no aliases */
    uint64_t fo=home->off+(va-home->start);unsigned aliases=0;
    for(unsigned i=0;i<watch_nmaps;i++){
        const WatchMap *m=&watch_maps[i];
        if(m->ino!=home->ino||m->dev!=home->dev)continue;
        uint64_t lo=fo>m->off?fo:m->off,hi=fo+len<m->off+(m->end-m->start)?fo+len:m->off+(m->end-m->start);
        if(lo>=hi)continue;
        if(!m->writable&&m!=home)continue;   /* a read-only view cannot write */
        if(++aliases>48)return 0;
        if(aliases>watch_max_aliases)watch_max_aliases=aliases;
        if(!watch_range_clean(m->start+(uintptr_t)(lo-m->off),(size_t)(hi-lo)))return 0;
    }
    return 1;
}

/* JOJO item 10: RECOMP_GL_TEXTURE_WATCH_FAST=1 (off by default). Same exact
 * soft-dirty test as watch_clean, checked on every use, but the alias list
 * for each texture slot is resolved once per maps reload instead of scanning
 * the maps table per use, pagemap is read in 512-page batches, and a lookup
 * miss reloads /proc/self/maps at most once per flip. Anything uncacheable
 * falls back to watch_clean. */
typedef struct { uintptr_t va;size_t len;unsigned gen;int n;uintptr_t s[16];size_t l[16]; } WatchAlias;
static WatchAlias watch_alias[1024];
static int watch_range_clean_big(uintptr_t va,size_t len)
{
    static uint64_t ent[512];
    uintptr_t first=va>>12,last=(va+len-1)>>12;
    for(uintptr_t p=first;p<=last;){
        size_t n=last-p+1;if(n>512)n=512;
        if(pread(watch_pagemap,ent,n*8,(off_t)(p*8))!=(ssize_t)(n*8))return 0;
        for(size_t i=0;i<n;i++)if(ent[i]&(1ull<<55))return 0;
        p+=n;
    }
    return 1;
}
static int watch_clean_slot(int slot,const void *ptr,size_t len)
{
    if(watch_fast<0){const char *v=getenv("RECOMP_GL_TEXTURE_WATCH_FAST");watch_fast=v&&v[0]=='1';}
    if(!watch_fast||slot<0||slot>=1024)return watch_clean(ptr,len);
    if(!watch_on||!watch_epoch||!len)return 0;
    WatchAlias *a=&watch_alias[slot];uintptr_t va=(uintptr_t)ptr;
    if(a->va!=va||a->len!=len||a->gen!=watch_maps_gen){
        a->va=va;a->len=len;a->gen=watch_maps_gen;a->n=-1;
        const WatchMap *home=NULL;
        for(unsigned i=0;i<watch_nmaps;i++)if(va>=watch_maps[i].start&&va<watch_maps[i].end){home=&watch_maps[i];break;}
        if(!home||va+len>home->end){a->gen=0;return watch_clean(ptr,len);}   /* not cached: slow path handles reload */
        if(!home->shared||!home->ino){a->n=1;a->s[0]=va;a->l[0]=len;}
        else {
            uint64_t fo=home->off+(va-home->start);int n=0;
            for(unsigned i=0;i<watch_nmaps;i++){
                const WatchMap *m=&watch_maps[i];
                if(m->ino!=home->ino||m->dev!=home->dev)continue;
                uint64_t lo=fo>m->off?fo:m->off,hi=fo+len<m->off+(m->end-m->start)?fo+len:m->off+(m->end-m->start);
                if(lo>=hi)continue;
                if(!m->writable&&m!=home)continue;
                if(n==16){n=-1;break;}
                a->s[n]=m->start+(uintptr_t)(lo-m->off);a->l[n]=(size_t)(hi-lo);n++;
            }
            a->n=n;
        }
    }
    if(a->n<0)return watch_clean(ptr,len);
    for(int i=0;i<a->n;i++)if(!watch_range_clean_big(a->s[i],a->l[i]))return 0;
    return 1;
}
