/* JOJO item 12: RECOMP_GL_TEXTURE_REUSE=1 (off by default). When a texture's
 * guest bytes change to content already decoded before (any address, same
 * format/size/sampler fields), copy the earlier GL texture on the GPU instead
 * of decoding and uploading again. A hit requires an identical key, size,
 * 64-bit hash AND a full byte compare against a stored copy, so results are
 * exactly those of a fresh decode. Paletted textures and texture packs are
 * never reused. 4-way x 256 sets, LRU, <=1 MiB per entry, 96 MiB total. */
typedef void (APIENTRY *ReuseCopyFn)(GLuint,GLenum,GLint,GLint,GLint,GLint,GLuint,GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei);
typedef struct { Nv2aTexture key;size_t span;uint64_t hash,used;unsigned char *bytes;GLuint tex; } ReuseEntry;
static ReuseEntry reuse_tab[256][4];static int reuse_on=-1;static ReuseCopyFn reuse_copy;
static uint64_t reuse_clock,reuse_bytes,reuse_hits,reuse_misses,reuse_stores,reuse_evicts;
static int reuse_enabled(const Nv2aTexture *t,size_t span)
{
    if(reuse_on<0){const char *v=getenv("RECOMP_GL_TEXTURE_REUSE");reuse_on=v&&v[0]=='1';
        if(reuse_on){reuse_copy=(ReuseCopyFn)SDL_GL_GetProcAddress("glCopyImageSubData");if(!reuse_copy)reuse_copy=(ReuseCopyFn)SDL_GL_GetProcAddress("glCopyImageSubDataARB");
            if(!reuse_copy){reuse_on=0;fprintf(stderr,"[GL] texture reuse: glCopyImageSubData unavailable, disabled\n");}
            else fprintf(stderr,"[GL] texture reuse enabled\n");}}
    return reuse_on>0&&span&&span<=(1u<<20);
}
static ReuseEntry *reuse_find(const Nv2aTexture *t,const unsigned char *raw,size_t span,uint64_t h)
{
    Nv2aTexture k=*t;k.offset=0;ReuseEntry *set=reuse_tab[h&255];
    for(int i=0;i<4;i++){ReuseEntry *c=&set[i];
        if(c->tex&&c->hash==h&&c->span==span&&!memcmp(&c->key,&k,sizeof(k))&&!memcmp(c->bytes,raw,span))return c;}
    return NULL;
}
/* e->id is bound. Returns 1 if e->id now holds the decoded image. */
static int reuse_try(const Nv2aTexture *t,const unsigned char *raw,size_t span,GLuint dst,uint64_t *hout)
{
    uint64_t h=texture_memory_hash(raw,span);*hout=h;
    ReuseEntry *c=reuse_find(t,raw,span,h);
    if(!c){reuse_misses++;return 0;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,t->width,t->height,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);
    reuse_copy(c->tex,GL_TEXTURE_2D,0,0,0,0,dst,GL_TEXTURE_2D,0,0,0,0,t->width,t->height,1);
    c->used=++reuse_clock;reuse_hits++;return 1;
}
static void reuse_store(const Nv2aTexture *t,const unsigned char *raw,size_t span,GLuint src,uint64_t h)
{
    if(reuse_find(t,raw,span,h))return;
    ReuseEntry *set=reuse_tab[h&255],*c=&set[0];
    for(int i=0;i<4;i++){if(!set[i].tex){c=&set[i];break;}if(set[i].used<c->used)c=&set[i];}
    while(reuse_bytes-(c->bytes?c->span:0)+span>(96u<<20)){ /* global LRU evict */
        ReuseEntry *o=NULL;for(int s=0;s<256;s++)for(int i=0;i<4;i++){ReuseEntry *x=&reuse_tab[s][i];if(x->bytes&&x!=c&&(!o||x->used<o->used))o=x;}
        if(!o)return;free(o->bytes);o->bytes=NULL;reuse_bytes-=o->span;o->span=0;o->hash=0;if(o->tex){glDeleteTextures(1,&o->tex);o->tex=0;}reuse_evicts++;}
    if(c->span!=span||!c->bytes){unsigned char *b=realloc(c->bytes,span);if(!b)return;reuse_bytes+=span-(c->bytes?c->span:0);c->bytes=b;}
    if(!c->tex)glGenTextures(1,&c->tex);
    Nv2aTexture k=*t;k.offset=0;c->key=k;c->span=span;c->hash=h;c->used=++reuse_clock;memcpy(c->bytes,raw,span);
    glBindTexture(GL_TEXTURE_2D,c->tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,t->width,t->height,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);
    reuse_copy(src,GL_TEXTURE_2D,0,0,0,0,c->tex,GL_TEXTURE_2D,0,0,0,0,t->width,t->height,1);
    glBindTexture(GL_TEXTURE_2D,src);reuse_stores++;
}
