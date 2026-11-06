/* The intro samples the shared NV2A Z24S8 surface as LIN_A8B8G8R8.
 * Rebuild its depth bytes on the GPU. The intro reads depth, not stencil. */
static GLuint ztex_fbo,ztex_vao,ztex_program[2];
static struct {GLuint color;uint32_t va,w,h;} ztex_slots[4];

static void ztex_reset(void)
{
    for(unsigned i=0;i<4;i++){
        GLuint id=ztex_slots[i].color;
        if(!id)continue;
        for(unsigned unit=0;unit<4;unit++)if(bound_textures[unit]==id)bound_textures[unit]=~0u;
        glDeleteTextures(1,&id);
    }
    memset(ztex_slots,0,sizeof(ztex_slots));
}

static GLuint zeta_as_texture(const Nv2aTexture *t)
{
    if(t->color!=0x3f||(t->pitch&&t->pitch!=t->width*4)||!ztex_enabled())return 0;
    GLuint depth=0;
    for(unsigned i=0;i<32;i++){
        if(depths[i].depth&&depths[i].va==t->offset&&depths[i].w==t->width&&depths[i].h==t->height){
            depth=depths[i].depth;
            break;
        }
    }
    if(!depth)return 0;

    unsigned slot=0;
    for(;slot<4;slot++)if(!ztex_slots[slot].color||ztex_slots[slot].va==t->offset)break;
    if(slot==4)slot=0;
    if(!ztex_slots[slot].color)glGenTextures(1,&ztex_slots[slot].color);
    GLuint color=ztex_slots[slot].color;
    unsigned w=t->width*scale,h=t->height*scale;
    if(ztex_slots[slot].w!=w||ztex_slots[slot].h!=h){
        texture_bind(color);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        ztex_slots[slot].w=w;ztex_slots[slot].h=h;
    }
    ztex_slots[slot].va=t->offset;
    unsigned multi=msaa>1;
    GLuint *pack=&ztex_program[multi];
    if(!*pack){
        static const char *vs="#version 330 core\n"
            "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
            "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
        char fs[768];
        snprintf(fs,sizeof(fs),"#version 330 core\nuniform %s depth_tex;\n"
            "out vec4 color;\nvoid main(){float d=texelFetch(depth_tex,ivec2(gl_FragCoord.xy),0).r;"
            "uint z=uint(round(clamp(d,0.0,1.0)*16777215.0));"
            "color=vec4(0.0,float(z&255u),float((z>>8)&255u),float(z>>16))/255.0;}\n",
            multi?"sampler2DMS":"sampler2D");
        GLuint vertex=shader(GL_VERTEX_SHADER,vs),fragment=shader(GL_FRAGMENT_SHADER,fs);
        *pack=glCreateProgram();
        glAttachShader(*pack,vertex);glAttachShader(*pack,fragment);glLinkProgram(*pack);
        glDeleteShader(vertex);glDeleteShader(fragment);
        GLint ok;
        glGetProgramiv(*pack,GL_LINK_STATUS,&ok);
        if(!ok)fatal("depth packing shader link");
        glUseProgram(*pack);
        glUniform1i(glGetUniformLocation(*pack,"depth_tex"),4);
    }
    if(!ztex_fbo){glGenFramebuffers(1,&ztex_fbo);glGenVertexArrays(1,&ztex_vao);}
    GLint draw_fbo,old_vao,viewport[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&old_vao);
    glGetIntegerv(GL_VIEWPORT,viewport);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,ztex_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)fatal("depth packing framebuffer");
    glViewport(0,0,w,h);
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    glUseProgram(*pack);glBindVertexArray(ztex_vao);
    GLenum target=multi?GL_TEXTURE_2D_MULTISAMPLE:GL_TEXTURE_2D;
    glActiveTexture(GL_TEXTURE4);glBindTexture(target,depth);
    glDrawArrays(GL_TRIANGLES,0,3);
    glBindTexture(target,0);glActiveTexture(GL_TEXTURE0+active_texture_unit);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo);
    glUseProgram(program);glBindVertexArray(old_vao);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    state_valid=0;
    static unsigned reported;
    unsigned bit=multi?2u:1u;
    if(!(reported&bit)){
        reported|=bit;
        fprintf(stderr,"[GL-ZTEX] %08X %ux%u at flip %llu (%dx samples)\n",
                t->offset,w,h,(unsigned long long)flips,msaa);
    }
    return color;
}
