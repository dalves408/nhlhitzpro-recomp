/* Native Linux host for the same generated Xbox guest and project fixes. */
#include <xbox/xboxrecomp.h>
#include <SDL2/SDL.h>
#include "gl_texture_pack.h"
#include "linux_performance.h"
#include <errno.h>
#include <fcntl.h>
#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/uio.h>

extern __thread uint32_t g_esp;
extern __thread uint32_t g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void backend_gl_register(void);
extern void xbox_OhciInit(void);
extern void xbox_path_init(const char *, const char *);
extern void xbox_WatchdogStart(void);
extern MCPXAPUState *g_apu_state;
extern int xbox_PosixMmioFault(void *, uintptr_t);

static void default_env(const char *key, const char *value)
{ if (!getenv(key)) setenv(key, value, 0); }

static void crash(int sig, siginfo_t *info, void *ctx)
{
    if (xbox_PosixMmioFault(ctx,(uintptr_t)info->si_addr)) return;
    ucontext_t *uc=ctx;const void *ip=(const void *)uc->uc_mcontext.gregs[REG_RIP];
    unsigned char code[6]={0};struct iovec local={code,sizeof(code)},remote={(void *)ip,sizeof(code)};
    (void)process_vm_readv(getpid(),&local,1,&remote,1,0);
    void *frames[48];
    dprintf(STDERR_FILENO, "[FATAL] signal=%d address=%p guest esp=%08X\n",
            sig, info->si_addr, g_esp);
    dprintf(STDERR_FILENO,"[FATAL] guest eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi);
    dprintf(STDERR_FILENO,"[FATAL] RIP=%p opcode=%02X %02X %02X %02X %02X %02X\n",ip,code[0],code[1],code[2],code[3],code[4],code[5]);
    backtrace_symbols_fd(frames, backtrace(frames, 48), STDERR_FILENO);
    const char *dump=getenv("RECOMP_CRASH_GUEST_DUMP");
    if(dump&&g_xbox_mem_offset){int fd=open(dump,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if(fd>=0){static unsigned char chunk[65536];
            for(uint32_t offset=0;offset<64u*1024*1024;offset+=sizeof(chunk)){
                memset(chunk,0,sizeof(chunk));struct iovec dst={chunk,sizeof(chunk)},src={(void *)((uintptr_t)g_xbox_mem_offset+offset),sizeof(chunk)};
                (void)process_vm_readv(getpid(),&dst,1,&src,1,0);
                size_t written=0;while(written<sizeof(chunk)){ssize_t n=write(fd,chunk+written,sizeof(chunk)-written);if(n<=0)break;written+=(size_t)n;}
                if(written<sizeof(chunk))break;
            }
            close(fd);
        }
    }
    _exit(128 + sig);
}

static int make_dirs(char *path)
{
    for (char *p=path+1; *p; ++p) if (*p=='/') {
        *p=0;
        if (mkdir(path,0700) && errno!=EEXIST) { *p='/'; return 0; }
        *p='/';
    }
    return !mkdir(path,0700) || errno==EEXIST;
}

int main(int argc, char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--build-info")){
        printf("{\"guest_opt_level\":%d}\n",HITZ_GUEST_OPT_LEVEL);return 0;
    }
    char root[PATH_MAX], game[PATH_MAX], xbe[PATH_MAX], saves[PATH_MAX];
    char *slash;
    ssize_t n=readlink("/proc/self/exe",root,sizeof(root)-1);
    if (n<=0) return 1;
    root[n]=0;
    /* Build executable is native/build-linux/<player>. */
    for (int i=0;i<3;i++) { slash=strrchr(root,'/'); if(!slash)return 1; *slash=0; }
    if (argc>1) {
        if (!realpath(argv[1],game)) { perror(argv[1]); return 1; }
    } else snprintf(game,sizeof(game),"%s/game_files",root);
    snprintf(xbe,sizeof(xbe),"%s/default.xbe",game);
    const char *data=getenv("XDG_DATA_HOME"), *home=getenv("HOME");
    if (data && *data) snprintf(saves,sizeof(saves),"%s/nhlhitzpro/SaveData",data);
    else if (home) snprintf(saves,sizeof(saves),"%s/.local/share/nhlhitzpro/SaveData",home);
    else { fprintf(stderr,"Set HOME or XDG_DATA_HOME for saves\n"); return 1; }
    if (!make_dirs(saves)) { perror(saves); return 1; }
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"=== NHL Hitz Pro — native Linux ===\nGame: %s\nSaves: %s\n",game,saves);
    FILE *f=fopen(xbe,"rb");
    if (!f) { perror(xbe); return 1; }
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    void *bytes=size>0?malloc((size_t)size):NULL;
    if (!bytes || fread(bytes,1,(size_t)size,f)!=(size_t)size) { fclose(f); free(bytes); return 1; }
    fclose(f);
    struct sigaction sa={0}; sa.sa_sigaction=crash; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask); sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL);
    sigaction(SIGABRT,&sa,NULL);sigaction(SIGILL,&sa,NULL);
    default_env("RECOMP_PB_EXEC","1"); default_env("RECOMP_PB_WALK","dma");
    default_env("RECOMP_UNIMPL_TRAP","1");default_env("RECOMP_STUB_LOG","1");
    default_env("RECOMP_AC97_READY","1"); default_env("RECOMP_USB","1");
    default_env("RECOMP_TITLE_KEVENTS","1"); default_env("RECOMP_KEYBOARD","1");
    default_env("RECOMP_DISPATCH_GATE","1"); default_env("RECOMP_DISC_BUFFERED","1");
    default_env("RECOMP_CONTIG_AVOID_IMAGE","1");
    default_env("RECOMP_VBLANK","paced"); default_env("RECOMP_GFX_THREAD","0");
    default_env("RECOMP_D3D11_RC","1");
    default_env("RECOMP_GPU_FFP","1"); default_env("RECOMP_D3D11_GPUVS","1");
    default_env("RECOMP_TEXDEC_FAST","1"); default_env("RECOMP_PB_BULK","1");
    default_env("RECOMP_NV_BLIT","1"); default_env("RECOMP_INLINE_SHORTFIX","1");
    default_env("RECOMP_INLINE_FFP","1"); default_env("RECOMP_FLIP_PACING","0");
    default_env("RECOMP_RENDER_PROGRESS","0"); default_env("RECOMP_KERNEL_SUMMARY","0");
    default_env("RECOMP_READ_LOG","0"); default_env("RECOMP_FB_PROBE","0");
    unsetenv("RECOMP_FB_WINDOW");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS,"1");
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); free(bytes); return 1;
    }
    xbox_InputInit();
    backend_gl_register();
    if (!xbox_MemoryLayoutInit(bytes,(size_t)size)) {
        fprintf(stderr,"Xbox memory mapping failed\n"); return 1;
    }
    g_xbox_mem_offset=xbox_GetMemoryOffset();
    g_apu_state=mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    xbox_OhciInit();
    xbox_kernel_init();
    xbox_path_init(game,saves);
    xbox_kernel_bridge_init();
    g_esp=XBOX_STACK_TOP;
    if (!recomp_dispatch_init()) fprintf(stderr,"[BOOT] using binary-search dispatch\n");
    xbox_WatchdogStart();
    fprintf(stderr,"[BOOT] starting Xbox entry at 001DB5E3, ESP=%08X\n",g_esp);
    xbe_entry_point();
    hitz_linux_performance_shutdown(); hitz_gl_pack_shutdown();
    xbox_kernel_shutdown(); xbox_MemoryLayoutShutdown(); free(bytes); SDL_Quit();
    return 0;
}
