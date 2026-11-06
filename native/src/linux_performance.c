/* Low-frequency, process-scoped metrics. No device-wide GPU percentage is
 * substituted for missing process telemetry (notably under WSLg). */
#include "linux_performance.h"
#include <SDL2/SDL.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int hitz_linux_cpu_percent(uint64_t old,uint64_t cpu,uint64_t wall,unsigned processors)
{
    if(!wall||!processors||cpu<old)return -1;
    double value=100.0*(double)(cpu-old)/(double)wall/processors;
    if(value>100)value=100;return (int)(value+.5);
}
void hitz_linux_gpu_parse(FILE *f,HitzGpuCounters *out)
{
    char line[256],device[48]="";uint64_t client=0;int have_client=0;
    HitzGpuCounter local[32];unsigned count=0;
    while(fgets(line,sizeof(line),f)){
        unsigned long long number;char engine[64],unit[16];
        if(sscanf(line,"drm-client-id: %llu",&number)==1){client=number;have_client=1;}
        else if(sscanf(line,"drm-pdev: %47s",device)==1){}
        else if(sscanf(line,"drm-engine-%63[^:]: %llu %15s",engine,&number,unit)==3&&!strcmp(unit,"ns")&&count<32){
            memset(&local[count],0,sizeof(local[count]));snprintf(local[count].engine,sizeof(local[count].engine),"%s",engine);local[count++].ns=number;
        }
    }
    if(!have_client||!*device)return;
    for(unsigned i=0;i<count&&out->count<HITZ_GPU_COUNTERS_MAX;i++){
        int duplicate=0;
        for(unsigned j=0;j<out->count;j++)if(out->value[j].client==client&&!strcmp(out->value[j].device,device)&&!strcmp(out->value[j].engine,local[i].engine)){duplicate=1;break;}
        if(!duplicate){local[i].client=client;snprintf(local[i].device,sizeof(local[i].device),"%s",device);out->value[out->count++]=local[i];}
    }
}
int hitz_linux_gpu_percent(const HitzGpuCounters *old,const HitzGpuCounters *cur,uint64_t elapsed)
{
    if(!elapsed)return -1;
    struct {char device[48],engine[64];double ns;} group[HITZ_GPU_COUNTERS_MAX];unsigned groups=0;int valid=0;
    for(unsigned i=0;i<cur->count;i++){
        const HitzGpuCounter *v=&cur->value[i];
        for(unsigned j=0;j<old->count;j++){
            const HitzGpuCounter *o=&old->value[j];
            if(o->client!=v->client||strcmp(o->device,v->device)||strcmp(o->engine,v->engine))continue;
            if(v->ns<o->ns)break;
            valid=1;unsigned g;
            for(g=0;g<groups;g++)if(!strcmp(group[g].device,v->device)&&!strcmp(group[g].engine,v->engine))break;
            if(g==groups){snprintf(group[g].device,sizeof(group[g].device),"%s",v->device);snprintf(group[g].engine,sizeof(group[g].engine),"%s",v->engine);group[g].ns=0;groups++;}
            group[g].ns+=(double)(v->ns-o->ns);break;
        }
    }
    if(!valid)return -1;
    double best=0;for(unsigned i=0;i<groups;i++){double percent=100*group[i].ns/(double)elapsed;if(percent>best)best=percent;}
    if(best>100)best=100;return (int)(best+.5);
}
static uint64_t clock_ns(clockid_t clock)
{struct timespec t;if(clock_gettime(clock,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
static void gpu_counters(HitzGpuCounters *out)
{
    out->count=0;DIR *d=opendir("/proc/self/fdinfo");if(!d)return;
    struct dirent *entry;unsigned files=0;
    while((entry=readdir(d))&&files<512){
        if(entry->d_name[0]=='.')continue;
        char *end;strtoul(entry->d_name,&end,10);if(*end)continue;
        char path[128];if(snprintf(path,sizeof(path),"/proc/self/fdinfo/%s",entry->d_name)>=(int)sizeof(path))continue;
        FILE *f=fopen(path,"r");if(f){hitz_linux_gpu_parse(f,out);fclose(f);}files++;
    }
    closedir(d);
}
static struct {SDL_mutex *mutex;SDL_cond *wake;SDL_Thread *thread;SDL_atomic_t gpu;int stop;uint64_t cpu,wall;unsigned processors;} perf;
static int gpu_monitor(void *unused)
{
    (void)unused;SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    HitzGpuCounters old={0},current;uint64_t previous=clock_ns(CLOCK_MONOTONIC);
    gpu_counters(&old);
    for(;;){
        SDL_LockMutex(perf.mutex);
        if(!perf.stop)SDL_CondWaitTimeout(perf.wake,perf.mutex,1000);
        int stop=perf.stop;SDL_UnlockMutex(perf.mutex);if(stop)break;
        uint64_t now=clock_ns(CLOCK_MONOTONIC);gpu_counters(&current);
        SDL_AtomicSet(&perf.gpu,hitz_linux_gpu_percent(&old,&current,now-previous));old=current;previous=now;
    }
    return 0;
}
void hitz_linux_performance_init(void)
{
    perf.cpu=clock_ns(CLOCK_PROCESS_CPUTIME_ID);perf.wall=clock_ns(CLOCK_MONOTONIC);
    long cpus=sysconf(_SC_NPROCESSORS_ONLN);perf.processors=cpus>0?(unsigned)cpus:1;
    SDL_AtomicSet(&perf.gpu,-1);perf.mutex=SDL_CreateMutex();perf.wake=SDL_CreateCond();
    if(perf.mutex&&perf.wake)perf.thread=SDL_CreateThread(gpu_monitor,"gpu-metrics",NULL);
}
void hitz_linux_performance_sample(int *cpu,int *gpu)
{
    uint64_t now=clock_ns(CLOCK_MONOTONIC),time=clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    *cpu=perf.wall&&now>=perf.wall&&time?hitz_linux_cpu_percent(perf.cpu,time,now-perf.wall,perf.processors):-1;
    *gpu=SDL_AtomicGet(&perf.gpu);perf.cpu=time;perf.wall=now;
}
void hitz_linux_performance_shutdown(void)
{
    if(perf.mutex){SDL_LockMutex(perf.mutex);perf.stop=1;if(perf.wake)SDL_CondBroadcast(perf.wake);SDL_UnlockMutex(perf.mutex);}
    if(perf.thread)SDL_WaitThread(perf.thread,NULL);
    if(perf.wake)SDL_DestroyCond(perf.wake);if(perf.mutex)SDL_DestroyMutex(perf.mutex);
    memset(&perf,0,sizeof(perf));
}
