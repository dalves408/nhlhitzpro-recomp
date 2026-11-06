/* JOJO item 11: RECOMP_FRAME_PROFILE=<csv> (off by default). Diagnostics only:
 * per flip, render-thread present sleep and swap time, push-buffer idle time
 * (render thread waiting for guest work), and run/runqueue-wait ms of every
 * process thread from /proc/self/task/<tid>/schedstat. Threads are listed at
 * flip 3000. Does not change any scheduling or guest timing. */
#include <dirent.h>
extern uint64_t jojo_pb_idle_ns __attribute__((weak));
static FILE *prof_csv;static int prof_on=-1,prof_n,prof_fd[48];static uint64_t prof_run[48],prof_wait[48];
static uint64_t prof_sleep_ns,prof_swap_ns,prof_idle_last;
static uint64_t prof_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static int prof_read(int fd,uint64_t *run,uint64_t *wait){char b[96];ssize_t n=pread(fd,b,sizeof(b)-1,0);if(n<=0)return 0;b[n]=0;unsigned long long r,w;if(sscanf(b,"%llu %llu",&r,&w)!=2)return 0;*run=r;*wait=w;return 1;}
static void prof_flip(double sec,double interval_ms)
{
    if(prof_on<0){const char *p=getenv("RECOMP_FRAME_PROFILE");prof_on=p&&*p;if(prof_on){prof_csv=fopen(p,"w");if(!prof_csv)prof_on=0;else setvbuf(prof_csv,NULL,_IOFBF,1<<16);}}
    if(!prof_on)return;
    if(!prof_n&&flips>=3000){
        DIR *d=opendir("/proc/self/task");struct dirent *de;fprintf(prof_csv,"frame,seconds,interval_ms,present_sleep_ms,swap_ms,pb_idle_ms");
        while(d&&(de=readdir(d))&&prof_n<48){if(de->d_name[0]=='.')continue;char p[96],c[32]={0};
            snprintf(p,sizeof p,"/proc/self/task/%s/comm",de->d_name);FILE *f=fopen(p,"r");if(f){if(fgets(c,sizeof c,f))c[strcspn(c,"\n")]=0;fclose(f);}
            for(char *q=c;*q;q++)if(*q==','||*q==' ')*q='_';
            snprintf(p,sizeof p,"/proc/self/task/%s/schedstat",de->d_name);int fd=open(p,O_RDONLY|O_CLOEXEC);if(fd<0)continue;
            prof_fd[prof_n]=fd;prof_read(fd,&prof_run[prof_n],&prof_wait[prof_n]);prof_n++;
            fprintf(prof_csv,",%s.%s_run,%s.%s_wait",c,de->d_name,c,de->d_name);}
        if(d)closedir(d);fputc('\n',prof_csv);
        if(&jojo_pb_idle_ns)prof_idle_last=jojo_pb_idle_ns;prof_sleep_ns=prof_swap_ns=0;return;
    }
    if(!prof_n)return;
    uint64_t idle=&jojo_pb_idle_ns?jojo_pb_idle_ns:0;
    fprintf(prof_csv,"%llu,%.6f,%.3f,%.3f,%.3f,%.3f",(unsigned long long)flips,sec,interval_ms,prof_sleep_ns/1e6,prof_swap_ns/1e6,(idle-prof_idle_last)/1e6);
    prof_idle_last=idle;prof_sleep_ns=prof_swap_ns=0;
    for(int i=0;i<prof_n;i++){uint64_t r=prof_run[i],w=prof_wait[i];prof_read(prof_fd[i],&r,&w);fprintf(prof_csv,",%.3f,%.3f",(r-prof_run[i])/1e6,(w-prof_wait[i])/1e6);prof_run[i]=r;prof_wait[i]=w;}
    fputc('\n',prof_csv);
}
