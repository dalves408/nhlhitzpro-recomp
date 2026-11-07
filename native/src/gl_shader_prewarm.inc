/* JOJO item 6: RECOMP_GL_SHADER_PREWARM=<file>. Records every GPU vertex program and
 * specialized combiner program the game builds, and on the next launch compiles them
 * all during init (before the menus) so match entry does not stall on compiles.
 * Off when unset. The file is plain text and safe to delete. */
static FILE *prewarm_out;static int prewarm_replaying;
static int prewarm_vs_index(GLuint vs){if(vs==vertex_shader)return -1;
    for(unsigned i=0;i<vertex_program_count;i++)if(vertex_programs[i].shader==vs)return (int)i;return -2;}
static void prewarm_note_vs(const Nv2aGpuVs *e){
    if(!prewarm_out||prewarm_replaying)return;
    fprintf(prewarm_out,"V %u %u %u %u",e->fixed_function,e->hash,e->nslots,e->input_mask);
    for(uint32_t i=0;i<e->nslots;i++)for(int j=0;j<4;j++)fprintf(prewarm_out," %x",e->prog[i][j]);
    fputc('\n',prewarm_out);fflush(prewarm_out);}
static void prewarm_note_program(const uint32_t *key,GLuint vs){
    if(!prewarm_out||prewarm_replaying)return;int vi=prewarm_vs_index(vs);if(vi<-1)return;
    fprintf(prewarm_out,"P %d",vi);for(int i=0;i<36;i++)fprintf(prewarm_out," %x",key[i]);
    fputc('\n',prewarm_out);fflush(prewarm_out);}
static void prewarm_load(void){
    const char *path=getenv("RECOMP_GL_SHADER_PREWARM");if(!path||!*path)return;
    FILE *f=fopen(path,"r");unsigned nv=0,np=0;Uint64 t=SDL_GetPerformanceCounter();
    GLuint vmap[64];unsigned vcount=0;
    if(f){char tag[2];prewarm_replaying=1;static Nv2aGpuVs e;
        while(fscanf(f," %1s",tag)==1){
            if(tag[0]=='V'){memset(&e,0,sizeof(e));if(fscanf(f,"%u %u %u %u",&e.fixed_function,&e.hash,&e.nslots,&e.input_mask)!=4||e.nslots>NV2A_VSH_SLOTS_MAX)break;
                for(uint32_t i=0;i<e.nslots;i++)for(int j=0;j<4;j++)if(fscanf(f,"%x",&e.prog[i][j])!=1)goto done;
                GLuint vs=gpu_vertex_shader(&e);if(vcount<64)vmap[vcount++]=vs;nv++;}
            else if(tag[0]=='P'){int vi;uint32_t k[36];if(fscanf(f,"%d",&vi)!=1)break;
                for(int i=0;i<36;i++)if(fscanf(f,"%x",&k[i])!=1)goto done;
                GLuint vs=vi<0?vertex_shader:(unsigned)vi<vcount?vmap[vi]:0;if(!vs)continue;
                Nv2aCombiner rc;memset(&rc,0,sizeof(rc));rc.control=k[0];rc.stage_program=k[1];rc.final0=k[2];rc.final1=k[3];
                for(int i=0;i<8;i++){rc.color_icw[i]=k[4+4*i];rc.alpha_icw[i]=k[5+4*i];rc.color_ocw[i]=k[6+4*i];rc.alpha_ocw[i]=k[7+4*i];}
                unsigned before=program_count;select_program(&rc,vs);if(program_count>before)np++;}
            else break;}
done:   fclose(f);prewarm_replaying=0;glFinish();
        active_program=&programs[0];program=programs[0].id;glUseProgram(program);}
    /* Rewrite the file: replayed entries first (same order), new ones appended. */
    prewarm_out=fopen(path,"a");
    fprintf(stderr,"[GL] shader prewarm: %u vertex + %u combiner programs in %.0f ms (%s)\n",nv,np,
        (SDL_GetPerformanceCounter()-t)*1000.0/SDL_GetPerformanceFrequency(),path);}
