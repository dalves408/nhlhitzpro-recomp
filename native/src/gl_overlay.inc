/* JOJO item 14: the Windows build's Esc "Do you want to quit?" prompt and F1
 * settings menu, drawn with OpenGL for the Linux backend. While one is open
 * the game is paused inside flip() (as backend_d3d11.c does) and the game
 * sees neutral pads (nhl_input_block). Settings are kept in the same
 * settings.ini the texture pack and the Linux config app use:
 * $XDG_DATA_HOME/nhlhitzpro/settings.ini (~/.local/share/...), [display].
 * An environment variable that is set still wins at start-up, as on Windows. */
#include <sys/stat.h>
#include <unistd.h>
#include "gl_overlay_font.h"
extern volatile int nhl_input_block;
extern void xbox_set_vblank_hz(unsigned hz);
enum{IT_RES,IT_FS,IT_STRETCH,IT_VSYNC,IT_FPS,IT_AA,IT_ANISO,IT_HIRES,IT_CLOSE,IT_QUIT,SET_ITEMS};
static const unsigned SET_FPS_HZ[6]={30,60,120,144,240,0};
static const char *const SET_LABEL[SET_ITEMS]={"Render Scale","Fullscreen","Stretch image","VSync","Frame Rate","Anti-aliasing","Anisotropic filter","Hi-res textures","Close menu","Quit game"};
static const char *const SET_RES[5]={"1x  640x480 (Native)","2x  1280x960","3x  1920x1440","4x  2560x1920","5x  3200x2400"};
static const char *const SET_OFFON[2]={"Off","On"};
static const char *const SET_ANISO[5]={"Off","2x","4x","8x","16x"};
static const char *const SET_STRETCH[2]={"Off (4:3)","On (fills window)"};
static const char *const SET_AA[4]={"Off","2x MSAA","4x MSAA","8x MSAA"};
static const char *const SET_FPS[6]={"30","60 (original)","120","144","240","Uncapped"};
static int ov_mode;            /* 0 none, 1 quit prompt, 2 settings, 3 restart prompt (item 25) */
static char ov_cwd[4096];
static int ov_qsel,ov_sel,ov_armed;
static int s_stretch,s_vsync,s_fpsi=1,s_hires,s_hires_boot,s_scale_cfg=1;

static int ov_cfg_path(char *o,size_t n)
{
    const char *x=getenv("XDG_DATA_HOME"),*h=getenv("HOME");char d[3500];
    if(x&&*x)snprintf(d,sizeof d,"%s/nhlhitzpro",x);else if(h&&*h)snprintf(d,sizeof d,"%s/.local/share/nhlhitzpro",h);else return 0;
    mkdir(d,0755);snprintf(o,n,"%s/settings.ini",d);return 1;
}
static int cfg_get_s(const char *sec,const char *key,int def)
{
    char path[4096],line[512],cur[80]="";FILE *f;int v=def;
    if(!ov_cfg_path(path,sizeof path)||!(f=fopen(path,"r")))return def;
    while(fgets(line,sizeof line,f)){char *s=line,k[80];int x;while(*s==' '||*s=='\t')s++;
        if(*s=='['){char *e=strchr(++s,']');if(e){*e=0;snprintf(cur,sizeof cur,"%s",s);}continue;}
        if(strcasecmp(cur,sec)||sscanf(s,"%79[^=]=%d",k,&x)!=2)continue;
        size_t n=strlen(k);while(n&&(k[n-1]==' '||k[n-1]=='\t'))k[--n]=0;if(!strcasecmp(k,key))v=x;}
    fclose(f);return v;
}
static void cfg_put_s(const char *sec,const char *key,int val)
{
    char path[4096],tmp[4200],line[512],cur[80]="";FILE *f,*o;int done=0,in=0;
    if(!ov_cfg_path(path,sizeof path))return;snprintf(tmp,sizeof tmp,"%s.tmp",path);
    if(!(o=fopen(tmp,"w")))return;
    if((f=fopen(path,"r"))){while(fgets(line,sizeof line,f)){char *s=line,k[80];int x;while(*s==' '||*s=='\t')s++;
        if(*s=='['){if(in&&!done){fprintf(o,"%s=%d\n",key,val);done=1;}
            char c2[80];char *e=strchr(s+1,']');snprintf(c2,sizeof c2,"%.*s",e?(int)(e-s-1):0,s+1);snprintf(cur,sizeof cur,"%s",c2);in=!strcasecmp(cur,sec);fputs(line,o);continue;}
        if(in&&!done&&sscanf(s,"%79[^=]=%d",k,&x)==2){size_t n=strlen(k);while(n&&(k[n-1]==' '||k[n-1]=='\t'))k[--n]=0;
            if(!strcasecmp(k,key)){fprintf(o,"%s=%d\n",key,val);done=1;continue;}}
        fputs(line,o);}fclose(f);}
    if(!done){if(in)fprintf(o,"%s=%d\n",key,val);else fprintf(o,"\n[%s]\n%s=%d\n",sec,key,val);}
    fclose(o);rename(tmp,path);
}
static int cfg_get(const char *k,int d){return cfg_get_s("display",k,d);}
static void cfg_put(const char *k,int v){cfg_put_s("display",k,v);}
/* item 25: remember which variables came from settings.ini so a restart hands the child the launch env, not stale values */
static const char *env_injected[16];static int env_injected_n;
static void env_default(const char *name,int v){char b[32];snprintf(b,sizeof b,"%d",v);
    if(!getenv(name)&&env_injected_n<16)env_injected[env_injected_n++]=name;setenv(name,b,0);}
static int fps_index(int hz){int best=0,bd=1<<30;if(hz>=360)hz=0;for(int i=0;i<6;i++){int d=abs((int)SET_FPS_HZ[i]-hz);if(d<bd){bd=d;best=i;}}return best;}
/* Before init() reads its environment: settings.ini fills in what is not set. */
static void ov_settings_load(void)
{
    if(!getcwd(ov_cwd,sizeof ov_cwd))ov_cwd[0]=0;
    int present_env=getenv("RECOMP_PRESENT_HZ")!=NULL;
    env_default("RECOMP_RENDER_SCALE",cfg_get("resolution_scale",1));
    env_default("RECOMP_GL_FULLSCREEN",cfg_get("fullscreen",0)!=0);
    env_default("RECOMP_GL_VSYNC",cfg_get("vsync",0)!=0);
    env_default("RECOMP_ANISOTROPY",cfg_get("anisotropy",1));
    env_default("RECOMP_GL_MSAA",cfg_get("msaa",1));
    {int hz=cfg_get("frame_rate",60);s_fpsi=fps_index(hz);
     if(!present_env){env_default("RECOMP_PRESENT_HZ",(int)SET_FPS_HZ[s_fpsi]);
        if(SET_FPS_HZ[s_fpsi]&&SET_FPS_HZ[s_fpsi]!=60)xbox_set_vblank_hz(SET_FPS_HZ[s_fpsi]);}}
    s_stretch=cfg_get("stretch",0)!=0;
    s_hires=getenv("RECOMP_HIRES")?atoi(getenv("RECOMP_HIRES"))!=0:cfg_get_s("textures","enabled",0)!=0;s_hires_boot=s_hires;
    {const char *rs=getenv("RECOMP_RENDER_SCALE");int sc=rs?atoi(rs):cfg_get("resolution_scale",1);if(sc<2&&s_hires){s_hires=0;cfg_put_s("textures","enabled",0);}}   /* item 27: 1x means hi-res off */
}
static int msaa_clamp(int want)
{   /* the most samples, up to the choice, the context supports (as msaa_effective) */
    GLint mx=1;glGetIntegerv(GL_MAX_SAMPLES,&mx);
    if(ztex_enabled()){
        GLint depth_max=1;
        glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES,&depth_max);
        if(depth_max<mx)mx=depth_max;
    }
    int n=want>=8?8:want>=4?4:want>=2?2:1;while(n>1&&n>mx)n>>=1;return n;
}
static void ov_settings_after_init(void)
{
    {const char *m=getenv("RECOMP_GL_MSAA");msaa=msaa_want=msaa_clamp(m?atoi(m):1);if(msaa>1)fprintf(stderr,"[GL-MSAA] %dx\n",msaa);}
    const char *v=getenv("RECOMP_GL_VSYNC");s_vsync=v&&atoi(v)!=0;
    s_fpsi=fps_index((int)present_hz);s_scale_cfg=(int)scale;
    {const char *s=getenv("RECOMP_RENDER_SCALE");if(s&&atoi(s)>=1&&atoi(s)<=5)s_scale_cfg=atoi(s);}
}
static void ov_apply_swap(void)
{   /* the JOJO item 5 cap-sync choice follows the frame rate, as at start-up */
    const char *cs=getenv("RECOMP_GL_CAP_SYNC");int c=cs?atoi(cs):0;
    int n=present_hz>=59&&present_hz<=61?1:present_hz>=29&&present_hz<=31?2:0;
    cap_sync=0;SDL_GL_SetSwapInterval(s_vsync);
    if(c&&n){int iv=c<0?-n:n;if(SDL_GL_SetSwapInterval(iv)!=0&&iv<0)SDL_GL_SetSwapInterval(iv=n);cap_sync=iv;}
    next_present_ns=0;
}
static int aniso_index(void){return anisotropy>=16?4:anisotropy>=8?3:anisotropy>=4?2:anisotropy>=2?1:0;}
/* item 27: at Render Scale 1x (or with no pack) hi-res is off, dark and skipped, as backend_d3d11.c */
static int set_disabled(int i){return i==IT_HIRES&&(s_scale_cfg<2||!hitz_gl_pack_available(5));}
static int set_count(int i)
{
    switch(i){case IT_RES:case IT_ANISO:return 5;case IT_FPS:return 6;
    case IT_AA:return 4;case IT_FS:case IT_STRETCH:case IT_VSYNC:case IT_HIRES:return 2;}return 0;
}
static int set_get(int i)
{
    switch(i){case IT_RES:return s_scale_cfg-1;case IT_FS:return (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN_DESKTOP)?1:0;
    case IT_STRETCH:return s_stretch;case IT_VSYNC:return s_vsync;case IT_FPS:return s_fpsi;case IT_ANISO:return aniso_index();case IT_AA:return msaa_want>=8?3:msaa_want>=4?2:msaa_want>=2?1:0;case IT_HIRES:return s_hires;}
    return 0;
}
static const char *set_text(int i)
{
    static char b[96];int v=set_get(i);
    switch(i){
    case IT_RES:return SET_RES[v];
    case IT_STRETCH:return SET_STRETCH[v];case IT_FPS:return SET_FPS[v];case IT_ANISO:return SET_ANISO[v];
    case IT_FS:case IT_VSYNC:return SET_OFFON[v];case IT_AA:return SET_AA[v];
    case IT_HIRES:{   /* JOJO item 24: plain On/Off when it matches this session's pack state */
        if(s_scale_cfg<2)return "Off (Needs 2x+)";   /* item 27: everything applies live */
        if(!hitz_gl_pack_available(5))return "Off (no pack)";
        return SET_OFFON[v];}}
    return "";
}
static void set_put(int i,int v)
{
    switch(i){
    case IT_RES:s_scale_cfg=v+1;cfg_put("resolution_scale",s_scale_cfg);scale_want=(unsigned)s_scale_cfg;
        if(s_scale_cfg<2&&s_hires){s_hires=0;cfg_put_s("textures","enabled",0);hitz_gl_pack_set_live(0);textures_flush();}break;   /* 1x turns hi-res off */   /* item 27: targets rebuilt after this frame */
    case IT_FS:SDL_SetWindowFullscreen(window,v?SDL_WINDOW_FULLSCREEN_DESKTOP:0);cfg_put("fullscreen",v);break;
    case IT_STRETCH:s_stretch=v;cfg_put("stretch",v);break;
    case IT_VSYNC:s_vsync=v;cfg_put("vsync",v);ov_apply_swap();break;
    case IT_FPS:s_fpsi=v;present_hz=SET_FPS_HZ[v];cfg_put("frame_rate",(int)SET_FPS_HZ[v]);xbox_set_vblank_hz(SET_FPS_HZ[v]?SET_FPS_HZ[v]:60);ov_apply_swap();break;
    case IT_ANISO:{float a=v?(float)(1<<v):1.0f,mx=1;
        if(epoxy_gl_version()>=46||epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic"))glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&mx);
        anisotropy=a>mx?mx:a;aniso_dirty=1;cfg_put("anisotropy",v?1<<v:1);break;}
    case IT_AA:{int n=v?1<<v:1;cfg_put("msaa",n);msaa_want=msaa_clamp(n);break;}   /* applied after this frame */
    case IT_HIRES:s_hires=v;cfg_put_s("textures","enabled",v);if(hitz_gl_pack_available(5)){hitz_gl_pack_set_live(v);textures_flush();}break;   /* item 26: live, as on Windows */
    }
}
/* ---- drawing: one small program, quads from two alpha atlases ---- */
typedef struct{float x,y,u,v,r,g,b,a;}OvVert;
static GLuint ov_prog,ov_vao,ov_vbo,ov_tex[2];static GLint ov_usz;
static OvVert ov_buf[24000];static int ov_n,ov_cur;
static float ov_sw,ov_sh;
static void ov_gl_init(void)
{
    if(ov_prog)return;
    static const char *vsrc="#version 330 core\nlayout(location=0) in vec2 p;layout(location=1) in vec2 t;layout(location=2) in vec4 c;uniform vec2 sz;out vec2 vt;out vec4 vc;\n"
        "void main(){vt=t;vc=c;gl_Position=vec4(p.x/sz.x*2.0-1.0,1.0-p.y/sz.y*2.0,0.0,1.0);}\n";
    static const char *fsrc="#version 330 core\nin vec2 vt;in vec4 vc;uniform sampler2D tx;out vec4 o;\nvoid main(){o=vec4(vc.rgb,vc.a*texture(tx,vt).r);}\n";
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs,1,&vsrc,NULL);glCompileShader(vs);glShaderSource(fs,1,&fsrc,NULL);glCompileShader(fs);
    ov_prog=glCreateProgram();glAttachShader(ov_prog,vs);glAttachShader(ov_prog,fs);glLinkProgram(ov_prog);
    {GLint ok=0;glGetProgramiv(ov_prog,GL_LINK_STATUS,&ok);if(!ok)fprintf(stderr,"[GL-OVERLAY] program link failed\n");}
    ov_usz=glGetUniformLocation(ov_prog,"sz");
    glGenVertexArrays(1,&ov_vao);glBindVertexArray(ov_vao);glGenBuffers(1,&ov_vbo);glBindBuffer(GL_ARRAY_BUFFER,ov_vbo);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(OvVert),(void*)0);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(OvVert),(void*)8);
    glEnableVertexAttribArray(2);glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(OvVert),(void*)16);
    glGenTextures(2,ov_tex);
    for(int i=0;i<2;i++){glBindTexture(GL_TEXTURE_2D,ov_tex[i]);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,i?font_body_w:font_title_w,i?font_body_h:font_title_h,0,GL_RED,GL_UNSIGNED_BYTE,i?font_body_px:font_title_px);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);}
}
static void ov_flush(void)
{
    if(!ov_n)return;glBindTexture(GL_TEXTURE_2D,ov_tex[ov_cur]);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(ov_n*sizeof(OvVert)),ov_buf,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,ov_n);ov_n=0;
}
static void ov_quad(int font,float x0,float y0,float x1,float y1,float u0,float v0,float u1,float v1,unsigned rgb,float a)
{
    if(font!=ov_cur){ov_flush();ov_cur=font;}if(ov_n+6>(int)(sizeof ov_buf/sizeof ov_buf[0]))ov_flush();
    float r=((rgb>>16)&255)/255.f,g=((rgb>>8)&255)/255.f,b=(rgb&255)/255.f;
    OvVert q[4]={{x0,y0,u0,v0,r,g,b,a},{x1,y0,u1,v0,r,g,b,a},{x1,y1,u1,v1,r,g,b,a},{x0,y1,u0,v1,r,g,b,a}};
    static const int ix[6]={0,1,2,0,2,3};for(int i=0;i<6;i++)ov_buf[ov_n++]=q[ix[i]];
}
#define FONT_W(f) ((f)?font_body_w:font_title_w)
#define FONT_H(f) ((f)?font_body_h:font_title_h)
#define FONT_CW(f) ((f)?font_body_cw:font_title_cw)
#define FONT_CH(f) ((f)?font_body_ch:font_title_ch)
#define FONT_ADV(f) ((f)?font_body_adv:font_title_adv)
static void ov_rect(float x0,float y0,float x1,float y1,unsigned rgb,float a)
{   /* the solid block in the body atlas's last cell */
    float cw=font_body_cw,ch=font_body_ch,cx=(95%16)*cw+cw/2,cy=(95/16)*ch+ch/2;
    ov_quad(1,x0,y0,x1,y1,cx/font_body_w,cy/font_body_h,cx/font_body_w,cy/font_body_h,rgb,a);
}
static float ov_text_w(int f,const char *s,float px){float w=0;for(;*s;s++){int c=(unsigned char)*s;if(c<32||c>126)c='?';w+=FONT_ADV(f)[c-32];}return w*px/48.f;}
/* item 25: the largest size <= px at which s fits in maxw (glyph overhang included) */
static float ov_fit(int f,const char *s,float px,float maxw){float w=ov_text_w(f,s,px)+px/12.f;return (w>maxw&&w>0)?px*maxw/w:px;}
/* align: 0 left, 1 center, 2 right, in the box x0..x1; vertically centred on cy */
static void ov_text(int f,const char *s,float px,float x0,float x1,float cy,int align,unsigned rgb)
{
    float k=px/48.f,w=ov_text_w(f,s,px),x=align==0?x0:align==1?(x0+x1-w)/2:x1-w;
    float cw=FONT_CW(f),ch=FONT_CH(f),y=cy-ch*k/2;
    for(;*s;s++){int c=(unsigned char)*s;if(c<32||c>126)c='?';int i=c-32;float u=(i%16)*cw,v=(i/16)*ch;
        if(c!=' ')ov_quad(f,x-2*k,y,x+(cw-2)*k,y+ch*k,u/FONT_W(f),v/FONT_H(f),(u+cw-2)/FONT_W(f),(v+ch)/FONT_H(f),rgb,1);
        x+=FONT_ADV(f)[i]*k;}
}
static void ov_draw(int cw,int ch)
{
    ov_gl_init();ov_sw=(float)cw;ov_sh=(float)ch;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glViewport(0,0,cw,ch);
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);
    glColorMask(1,1,1,1);glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ov_prog);glUniform2f(ov_usz,(float)cw,(float)ch);glUniform1i(glGetUniformLocation(ov_prog,"tx"),0);
    glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);glBindVertexArray(ov_vao);glBindBuffer(GL_ARRAY_BUFFER,ov_vbo);ov_n=0;ov_cur=1;
    ov_rect(0,0,(float)cw,(float)ch,0,150/255.f);                         /* dim: black, alpha 150 */
    if(ov_mode==1||ov_mode==3){int rs=ov_mode==3;
        const char *msg=rs?"Restart now to apply changes?":"Do you want to quit?";
        float dw=ch*4/5.f;if(dw>cw*0.9f)dw=cw*0.9f;float dh=dw*9/20,pad=dh*0.12f,tp=dh*0.17f*1.25f;
        {float need=ov_text_w(1,msg,tp)+tp/12.f+2*pad;if(need>dw)dw=need<cw*0.95f?need:cw*0.95f;}   /* grow to the text, then clamp */
        tp=ov_fit(1,msg,tp,dw-2*pad);
        float dx=(cw-dw)/2,dy=(ch-dh)/2,in=dh/40+2;
        ov_rect(dx,dy,dx+dw,dy+dh,0xFF7A00,1);ov_rect(dx+in,dy+in,dx+dw-in,dy+dh-in,0x0E1628,1);
        ov_text(1,msg,tp,dx+pad,dx+dw-pad,dy+dh*0.31f,1,0xFFFFFF);
        float bw=dh*2/3,bh=dh*0.28f,by=dy+dh*0.58f;
        for(int i=0;i<2;i++){float bx=i?dx+dw/2+dh/20:dx+dw/2-bw-dh/20;int on=ov_qsel==i;
            ov_rect(bx,by,bx+bw,by+bh,on?0xFF7A00:0x343C4E,1);{const char *bl=i?"YES":rs?"LATER":"NO";ov_text(1,bl,ov_fit(1,bl,dh*0.15f*1.25f,bw-bh*0.4f),bx,bx+bw,by+bh/2,1,on?0x0A0A0E:0xC8D2E1);}}
    }else{
        float rh=ch*7/100.f;if(rh<22)rh=22;float dw=rh*17;if(dw>cw*0.95f){dw=cw*0.95f;rh=dw/17;}
        float dh=rh*13.3f,dx=(cw-dw)/2,dy=(ch-dh)/2,in=dh/40+2;
        ov_rect(dx,dy,dx+dw,dy+dh,0xFF7A00,1);ov_rect(dx+in,dy+in,dx+dw-in,dy+dh-in,0x0E1628,1);
        ov_text(0,"SETTINGS",ov_fit(0,"SETTINGS",rh*0.8f*1.05f,dw-2*rh/3),dx,dx+dw,dy+rh/4+rh*0.525f,1,0xFFFFFF);
        for(int i=0;i<SET_ITEMS;i++){int sel=i==ov_sel,dis=set_disabled(i);char buf[128];
            float y=dy+rh*1.4f+i*rh,x0=dx+rh/3,x1=dx+dw-rh/3,y1=y+rh*0.82f;
            ov_rect(x0,y,x1,y1,sel?0xFF7A00:dis?0x1C222E:0x343C4E,1);
            unsigned tc=sel?0x0A0A0E:dis?0x5C6472:0xC8D2E1;
            float tp=rh*0.56f*1.15f,lw;tp=ov_fit(1,SET_LABEL[i],tp,(x1-x0)*0.45f);lw=ov_text_w(1,SET_LABEL[i],tp);
            ov_text(1,SET_LABEL[i],tp,x0+rh/3,x1,(y+y1)/2,0,tc);
            if(set_count(i))snprintf(buf,sizeof buf,"<  %s  >",set_text(i));
            else if(i==IT_QUIT&&ov_armed)snprintf(buf,sizeof buf,"press again to quit");
            else if(i==IT_HIRES||set_disabled(i))snprintf(buf,sizeof buf,"%s",set_text(i));
            else buf[0]=0;
            {float avail=(x1-rh/3)-(x0+rh/3+lw+rh/2);ov_text(1,buf,ov_fit(1,buf,rh*0.56f*1.15f,avail),x0,x1-rh/3,(y+y1)/2,2,tc);}}
        {float hy=dy+rh*1.4f+SET_ITEMS*rh+rh*0.35f,hs=rh*0.38f*1.15f;
        static const char *h1="Up/Down choose, Left/Right change, Enter or A confirms.",*h2="Saved automatically. F1, Esc or B closes.";
        float hw=dw-2*rh/3,h=ov_fit(1,h1,hs,hw);{float h2s=ov_fit(1,h2,hs,hw);if(h2s<h)h=h2s;}
        ov_text(1,h1,h,dx+rh/3,dx+dw-rh/3,hy,0,0x96A2B6);ov_text(1,h2,h,dx+rh/3,dx+dw-rh/3,hy+rh*0.48f,0,0x96A2B6);}
    }
    ov_flush();
    /* hand the backend its state back: everything is re-bound by the next draw */
    glDisable(GL_BLEND);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBindVertexArray(vao);
    program=0;state_valid=0;bound_framebuffer=~0u;viewport_w=viewport_h=0;pack_invalidate_bindings(0);for(int i=0;i<4;i++)bound_samplers[i]=~0u;
}
extern void xbox_time_pause_begin(void),xbox_time_pause_end(void),xa2_set_muted(int);
/* item 27: as backend_d3d11.c quit_open/quit_close: the guest clocks stop and the sound is muted while an overlay is up */
static void ov_open(int m){if(!ov_mode){xbox_time_pause_begin();xa2_set_muted(1);fprintf(stderr,"[GL-OVERLAY] pause\n");}ov_mode=m;ov_qsel=0;ov_armed=0;if(m==2)ov_sel=0;nhl_input_block=1;}
static void ov_close(void){if(ov_mode){xa2_set_muted(0);xbox_time_pause_end();fprintf(stderr,"[GL-OVERLAY] resume\n");}ov_mode=0;nhl_input_block=0;next_present_ns=0;}
static void ov_quit(const char *why){fprintf(stderr,"[GL-OVERLAY] quit: %s\n",why);fflush(stderr);exit(0);}
static void ov_restart(void)
{   /* settings.ini is written on every change; shut down like exit() would, then exec ourselves */
    char *buf=NULL;size_t n=0,cap=0;FILE *f=fopen("/proc/self/cmdline","rb");
    if(f){int c;while((c=fgetc(f))!=EOF){if(n+1>=cap){cap=cap?cap*2:4096;buf=realloc(buf,cap);}buf[n++]=(char)c;}fclose(f);}
    if(!buf||!n){fprintf(stderr,"[GL-OVERLAY] restart: no cmdline, closing menu\n");free(buf);ov_close();return;}
    buf[n]=0;int argc=0;for(size_t i=0;i<n;i++)if(!buf[i])argc++;
    char **av=calloc((size_t)argc+1,sizeof *av);{size_t i=0;for(int a=0;a<argc;a++){av[a]=buf+i;i+=strlen(buf+i)+1;}}
    fprintf(stderr,"[GL-OVERLAY] restart: relaunching %s\n",av[0]);fflush(stdout);fflush(stderr);
    hitz_gl_pack_shutdown();hitz_linux_performance_shutdown();
    SDL_Quit();                                   /* closes audio device, GL context and window */
    for(int fd=3;fd<1024;fd++)close(fd);
    if(ov_cwd[0]&&chdir(ov_cwd)){}
    for(int i=0;i<env_injected_n;i++)unsetenv(env_injected[i]);   /* child re-reads settings.ini */
    execv("/proc/self/exe",av);
    fprintf(stderr,"[GL-OVERLAY] restart: exec failed, exiting\n");fflush(stderr);_exit(1);
}
static void ov_settings_close(void){ov_close();}   /* item 27: no option needs a restart (mode 3 / ov_restart unused) */
enum{OV_UP,OV_DOWN,OV_LEFT,OV_RIGHT,OV_OK,OV_BACK,OV_YES,OV_NO,OV_TAB};
static void ov_action(int a)
{
    if(ov_mode==3){   /* restart prompt: same keys/buttons as the quit prompt */
        if(a==OV_LEFT||a==OV_RIGHT||a==OV_TAB||a==OV_UP||a==OV_DOWN)ov_qsel^=1;
        else if(a==OV_OK){if(ov_qsel)ov_restart();ov_close();}
        else if(a==OV_YES)ov_restart();
        else if(a==OV_NO||a==OV_BACK)ov_close();
        return;
    }
    if(ov_mode==1){
        if(a==OV_LEFT||a==OV_RIGHT||a==OV_TAB||a==OV_UP||a==OV_DOWN)ov_qsel^=1;
        else if(a==OV_OK){if(ov_qsel)ov_quit("Yes in the quit prompt");ov_close();}
        else if(a==OV_YES)ov_quit("Y in the quit prompt");
        else if(a==OV_NO||a==OV_BACK)ov_close();
        return;
    }
    int n;
    switch(a){
    case OV_UP:ov_armed=0;do ov_sel=(ov_sel+SET_ITEMS-1)%SET_ITEMS;while(set_disabled(ov_sel));break;
    case OV_DOWN:case OV_TAB:ov_armed=0;do ov_sel=(ov_sel+1)%SET_ITEMS;while(set_disabled(ov_sel));break;
    case OV_LEFT:case OV_RIGHT:if((n=set_count(ov_sel))>0)set_put(ov_sel,(set_get(ov_sel)+(a==OV_LEFT?n-1:1))%n);break;
    case OV_OK:
        if(ov_sel==IT_CLOSE)ov_settings_close();
        else if(ov_sel==IT_QUIT){if(ov_armed)ov_quit("quit chosen in the settings overlay");ov_armed=1;}
        else if((n=set_count(ov_sel))>0)set_put(ov_sel,(set_get(ov_sel)+1)%n);
        break;
    case OV_BACK:ov_settings_close();break;
    }
}
/* 1 = the event belongs to the overlay (the game does not see it) */
static int ov_event(const SDL_Event *e)
{
    if(e->type==SDL_KEYDOWN){SDL_Keycode k=e->key.keysym.sym;
        if(!ov_mode){if(e->key.repeat)return 0;if(k==SDLK_ESCAPE){ov_open(1);return 1;}if(k==SDLK_F1){ov_open(2);return 1;}return 0;}
        if(k==SDLK_ESCAPE){if(ov_mode==2)ov_settings_close();else ov_close();return 1;}
        if(k==SDLK_F1){if(ov_mode==2)ov_settings_close();else if(ov_mode==3)ov_close();else ov_open(2);return 1;}
        switch(k){case SDLK_UP:ov_action(OV_UP);break;case SDLK_DOWN:ov_action(OV_DOWN);break;case SDLK_LEFT:ov_action(OV_LEFT);break;
        case SDLK_RIGHT:ov_action(OV_RIGHT);break;case SDLK_TAB:ov_action(OV_TAB);break;case SDLK_RETURN:case SDLK_KP_ENTER:case SDLK_SPACE:ov_action(OV_OK);break;
        case SDLK_y:ov_action(OV_YES);break;case SDLK_n:ov_action(OV_NO);break;}
        return 1;
    }
    if(e->type==SDL_KEYUP)return ov_mode!=0;
    if(ov_mode&&e->type==SDL_CONTROLLERBUTTONDOWN){
        switch(e->cbutton.button){case SDL_CONTROLLER_BUTTON_DPAD_UP:ov_action(OV_UP);break;case SDL_CONTROLLER_BUTTON_DPAD_DOWN:ov_action(OV_DOWN);break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:ov_action(OV_LEFT);break;case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:ov_action(OV_RIGHT);break;
        case SDL_CONTROLLER_BUTTON_A:ov_action(OV_OK);break;case SDL_CONTROLLER_BUTTON_B:case SDL_CONTROLLER_BUTTON_START:ov_action(OV_BACK);break;}
        return 0;   /* the pad driver still tracks its state; the game sees neutral pads */
    }
    return 0;
}
static void ov_blit_frame(Target *r,int w,int h)
{
    int vw=w,vh=w*3/4;if(vh>h){vh=h;vw=h*4/3;}if(s_stretch){vw=w;vh=h;}
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);bound_framebuffer=~0u;glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);state_valid=0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,r->fbo);glBlitFramebuffer(0,0,r->w*scale,r->h*scale,(w-vw)/2,(h+vh)/2,(w+vw)/2,(h-vh)/2,GL_COLOR_BUFFER_BIT,GL_LINEAR);
}
static void events(void);
/* The game is paused here while an overlay is open, as in backend_d3d11.c. */
static void ov_pause(Target *r)
{
    int shot=getenv("RECOMP_OVERLAY_SHOT")!=NULL;
    const char *hv=getenv("RECOMP_OVERLAY_HOLD_MS");Uint32 hold=hv?(Uint32)atoi(hv):0,t0=SDL_GetTicks();   /* test only: auto-close */
    while(ov_mode){if(hold&&SDL_GetTicks()-t0>hold){ov_close();break;}int w,h;SDL_GL_GetDrawableSize(window,&w,&h);
        if(r)ov_blit_frame(r,w,h);else{glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);}
        ov_draw(w,h);SDL_GL_SwapWindow(window);SDL_Delay(8);events();
        if(shot&&ov_mode)SDL_Delay(1);}
    if(aniso_dirty)state_valid=0;
}
