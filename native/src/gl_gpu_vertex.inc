/* Native GPU vertex execution. The executor owns and packs each input block;
 * no guest memory is retained after draw(). Transform feedback returns the
 * same 29-float record the independent CPU verifier already checks. */
typedef struct {GLuint shader;uint32_t hash,fixed,nslots,prog[NV2A_VSH_SLOTS_MAX][4];} GlVertexProgram;
static GlVertexProgram vertex_programs[64];
static unsigned vertex_program_count;
static GLuint raw_buffer,feedback_buffer;
static struct {
    GLuint buffer;unsigned char *mapped;size_t alignment,chunk_bytes,used;
    unsigned chunk;int attempted;GLsync fences[4];uint64_t wraps;
} vertex_stream;
static const char *vertex_upload_name(int mode)
{return mode==2?"stream":mode==1?"direct":"legacy";}
static int vertex_index_mode(void)
{
    static int mode=-1;static const char *control;static uint64_t checked;
    if(mode<0){const char *v=getenv("RECOMP_GL_VERTEX_INDEXED");mode=v&&atoi(v)!=0;
        control=getenv("RECOMP_GL_VERTEX_INDEX_CONTROL");
        fprintf(stderr,"[GL-VERTEX-INDEX] %s\n",mode?"indexed":"arrays");}
    if(control){uint64_t now=SDL_GetTicks64();if(now-checked>=1000){
        checked=now;FILE *f=fopen(control,"r");char text[24];
        if(f){if(fgets(text,sizeof(text),f)){
            int next=!strncmp(text,"indexed",7)?1:!strncmp(text,"arrays",6)?0:mode;
            if(next!=mode){mode=next;fprintf(stderr,"[GL-VERTEX-INDEX] %s\n",mode?"indexed":"arrays");}
        }fclose(f);}
    }}
    return mode;
}
/* Optional developer control is sampled once a second, never per draw I/O.
 * Both paths replace the buffer's storage, retaining synchronous input ownership. */
static int vertex_upload_mode(void)
{
    static int mode=-1;static const char *control;static uint64_t checked;
    if(mode<0){const char *v=getenv("RECOMP_GL_VERTEX_UPLOAD");
        mode=v&&!strcmp(v,"stream")?2:v&&!strcmp(v,"direct")?1:0;control=getenv("RECOMP_GL_VERTEX_UPLOAD_CONTROL");
        fprintf(stderr,"[GL-VERTEX-UPLOAD] %s\n",vertex_upload_name(mode));}
    if(control){uint64_t now=SDL_GetTicks64();if(now-checked>=1000){
        checked=now;FILE *f=fopen(control,"r");char text[24];
        if(f){if(fgets(text,sizeof(text),f)){
            int next=!strncmp(text,"stream",6)?2:!strncmp(text,"direct",6)?1:!strncmp(text,"legacy",6)?0:mode;
            if(next!=mode){mode=next;fprintf(stderr,"[GL-VERTEX-UPLOAD] %s\n",vertex_upload_name(mode));}
        }fclose(f);}
    }}
    return mode;
}

static int vertex_stream_upload(const Nv2aGpuVs *e,GLintptr *offset)
{
    if(!vertex_stream.attempted){
        vertex_stream.attempted=1;
        if(epoxy_gl_version()<44&&!epoxy_has_gl_extension("GL_ARB_buffer_storage"))return 0;
        GLint alignment=0;glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT,&alignment);
        if(alignment<1||(4u<<20)%(unsigned)alignment)return 0;
        vertex_stream.alignment=(size_t)alignment;
        vertex_stream.chunk_bytes=4u<<20;
        const GLbitfield flags=GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT;
        glGenBuffers(1,&vertex_stream.buffer);glBindBuffer(GL_SHADER_STORAGE_BUFFER,vertex_stream.buffer);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER,vertex_stream.chunk_bytes*4,NULL,flags|GL_CLIENT_STORAGE_BIT);
        vertex_stream.mapped=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,vertex_stream.chunk_bytes*4,flags);
        if(!vertex_stream.mapped){
            fprintf(stderr,"[GL-VERTEX-STREAM] unavailable (GL error %x); legacy fallback\n",glGetError());
            glDeleteBuffers(1,&vertex_stream.buffer);vertex_stream.buffer=0;return 0;
        }
        fprintf(stderr,"[GL-VERTEX-STREAM] %zu bytes, four fenced chunks\n",vertex_stream.chunk_bytes*4);
    }
    size_t bytes=((size_t)e->block_bytes+3)&~(size_t)3;
    if(!vertex_stream.mapped||bytes>vertex_stream.chunk_bytes)return 0;
    size_t at=(vertex_stream.used+vertex_stream.alignment-1)/vertex_stream.alignment*vertex_stream.alignment;
    if(at+bytes>vertex_stream.chunk_bytes){
        vertex_stream.fences[vertex_stream.chunk]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if(!vertex_stream.fences[vertex_stream.chunk])glFinish();
        vertex_stream.chunk=(vertex_stream.chunk+1)%4;vertex_stream.wraps++;
        GLsync fence=vertex_stream.fences[vertex_stream.chunk];
        if(fence){GLenum result;
            do{result=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,1000000);}while(result==GL_TIMEOUT_EXPIRED);
            if(result==GL_WAIT_FAILED)fatal("vertex upload fence failed");
            glDeleteSync(fence);vertex_stream.fences[vertex_stream.chunk]=NULL;
        }
        vertex_stream.used=at=0;
    }
    *offset=(GLintptr)(vertex_stream.chunk*vertex_stream.chunk_bytes+at);
    if(bytes>e->block_bytes)memset(vertex_stream.mapped+*offset+e->block_bytes,0,bytes-e->block_bytes);
    memcpy(vertex_stream.mapped+*offset,e->block,e->block_bytes);
    vertex_stream.used=at+bytes;
    /* Coherent client writes are visible to subsequent draws. Fences protect
     * storage reuse, including when the mode is switched between frames. */
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER,0,vertex_stream.buffer,*offset,bytes);
    return 1;
}

static char *read_shader_file(const char *name)
{
    char path[4096];
    char executable[4096];
    FILE *f = NULL;
    ssize_t npath = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (npath > 0 && (size_t)npath < sizeof(executable) - 1) {
        executable[npath] = 0;
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = 0;
            if (snprintf(path, sizeof(path), "%s/shaders/%s", executable, name) < (int)sizeof(path))
                f = fopen(path, "rb");
        }
    }
    if (!f) {
        if (snprintf(path, sizeof(path), "%s/%s", HITZ_SHADER_DIR, name) >= (int)sizeof(path))
            return NULL;
        f = fopen(path, "rb");
    }
    if (!f) return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);
    if(n<0||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    char *p=malloc((size_t)n+1);
    if(p&&fread(p,1,(size_t)n,f)==(size_t)n)p[n]=0;
    else {free(p);p=NULL;}fclose(f);return p;
}
static void prewarm_note_vs(const Nv2aGpuVs *e);
static GLuint gpu_vertex_shader(const Nv2aGpuVs *e)
{
    init();if(epoxy_gl_version()<43||e->nslots>NV2A_VSH_SLOTS_MAX)return 0;
    for(unsigned i=0;i<vertex_program_count;i++){
        GlVertexProgram *v=&vertex_programs[i];
        if(v->fixed==e->fixed_function&&(v->fixed||(v->hash==e->hash&&v->nslots==e->nslots&&!memcmp(v->prog,e->prog,e->nslots*16))))return v->shader;
    }
    if(vertex_program_count>=64||program_count>=512)return 0;
    GlVertexProgram *v=&vertex_programs[vertex_program_count++];
    v->hash=e->hash;v->fixed=e->fixed_function;v->nslots=e->nslots;memcpy(v->prog,e->prog,e->nslots*16);
    char *wrapper=read_shader_file("gpu.vert"),*body=NULL;
    if(e->fixed_function)body=read_shader_file("ffp.glsl");
    else {body=malloc(65536);if(body&&!nv2a_vsh_gen_glsl(e,body,65536)){free(body);body=NULL;}}
    if(!wrapper||!body){free(wrapper);free(body);return 0;}
    char *marker=strstr(wrapper,"//@@BODY@@");
    if(!marker){free(wrapper);free(body);return 0;}
    *marker=0;const char *tail=marker+strlen("//@@BODY@@");
    size_t len=strlen(wrapper)+strlen(body)+strlen(tail)+1;char *src=malloc(len);
    if(!src){free(wrapper);free(body);return 0;}
    snprintf(src,len,"%s%s%s",wrapper,body,tail);free(wrapper);free(body);
    GLuint vs=glCreateShader(GL_VERTEX_SHADER);glShaderSource(vs,1,(const char *const *)&src,NULL);glCompileShader(vs);free(src);
    GLint ok;glGetShaderiv(vs,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[8192];glGetShaderInfoLog(vs,sizeof(log),NULL,log);fprintf(stderr,"[GL-GPUVS] CPU fallback: %s\n",log);glDeleteShader(vs);return 0;}
    v->shader=vs;prewarm_note_vs(e);
    /* Reserve a correct generic fragment fallback before specialization cache fills. */
    make_program(fragment_source,vs);
    fprintf(stderr,"[GL-GPUVS] shader=%u fixed=%u slots=%u\n",vertex_program_count,e->fixed_function,e->nslots);
    return vs;
}
int nv2a_gpuvs_probe(const Nv2aGpuVs *e){return gpu_vertex_shader(e)!=0;}

static void gpu_vertex_draw(const Nv2aGpuVs *e,uint32_t count)
{
    GLintptr offset=0;GLuint input_buffer=vertex_stream.buffer;
    int mode=vertex_upload_mode();
    if(mode==2&&vertex_stream_upload(e,&offset))input_buffer=vertex_stream.buffer;
    else {
        if(!raw_buffer)glGenBuffers(1,&raw_buffer);input_buffer=raw_buffer;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,raw_buffer);
        if(mode==1&&!(e->block_bytes&3u))
            glBufferData(GL_SHADER_STORAGE_BUFFER,e->block_bytes,e->block,GL_STREAM_DRAW);
        else {
            /* Odd byte spans retain padded storage without reading past the input. */
            glBufferData(GL_SHADER_STORAGE_BUFFER,(e->block_bytes+3u)&~3u,NULL,GL_STREAM_DRAW);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,e->block_bytes,e->block);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,raw_buffer);
    }
    int indexed=vertex_index_mode();uniform1ui(U_GINDEXED,indexed);
    uniform2f(U_GAA,e->aa_sx,e->aa_sy);uniform1ui(U_GFOGMODE,e->fog_mode);
    uniform1f(U_GFPX,e->fog_px);uniform1f(U_GFPY,e->fog_py);
    int verify=e->verify&&e->verify_out;
    if(e->verify_count)*e->verify_count=0;
    if(verify){if(!feedback_buffer)glGenBuffers(1,&feedback_buffer);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,feedback_buffer);glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,(size_t)count*29*4,NULL,GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,feedback_buffer);glBeginTransformFeedback(GL_TRIANGLES);}
    if(indexed){
        /* The packed list already contains actual guest vertex indices.
         * Reuse it as an element buffer so the GPU can cache transformed vertices. */
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,input_buffer);
        glDrawElements(GL_TRIANGLES,count,GL_UNSIGNED_INT,(const void *)(uintptr_t)(offset+GVS_OFF_IDX));
    }else glDrawArrays(GL_TRIANGLES,0,count);
    if(verify){glEndTransformFeedback();uint32_t n=count<4096?count:4096;
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,feedback_buffer);glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER,0,(size_t)n*29*4,e->verify_out);
        if(e->verify_count)*e->verify_count=n;glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);}
}
