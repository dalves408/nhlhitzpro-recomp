/* Cooperate with the host while the executor owns an outstanding PFB flush.
 * This never acknowledges hardware, advances GET, or changes guest registers.
 * The original guest backedge always rechecks the busy bit after returning. */
#include "linux_gpu_wait.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
extern ptrdiff_t xbox_GetMemoryOffset(void);
static unsigned waiters;
static int cooperative=-1;

int hitz_linux_gpu_wait_cooperative(void)
{
    int enabled=__atomic_load_n(&cooperative,__ATOMIC_RELAXED);
    if(enabled<0){const char *v=getenv("RECOMP_LINUX_GPU_WAIT");
        enabled=!(v&&!strcmp(v,"spin"));int expected=-1;
        __atomic_compare_exchange_n(&cooperative,&expected,enabled,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
        enabled=__atomic_load_n(&cooperative,__ATOMIC_RELAXED);}
    return enabled;
}
/* Called by the renderer's existing one-second diagnostic update. */
void hitz_linux_gpu_wait_poll(void)
{
    static const char *control;static int checked;
    if(!checked){control=getenv("RECOMP_LINUX_GPU_WAIT_CONTROL");checked=1;}
    if(!control)return;
    FILE *f=fopen(control,"r");char text[32];
    if(f){if(fgets(text,sizeof(text),f)){
        int previous=hitz_linux_gpu_wait_cooperative();
        int next=!strncmp(text,"cooperative",11)?1:!strncmp(text,"spin",4)?0:previous;
        if(next!=previous){__atomic_store_n(&cooperative,next,__ATOMIC_RELAXED);
            fprintf(stderr,"[LINUX-GPU-WAIT] %s\n",next?"cooperative":"spin");}
    }fclose(f);}
}
void hitz_linux_gpu_wait_hint(const volatile uint32_t *reg)
{
    if(!hitz_linux_gpu_wait_cooperative())return;
    /* Short requests usually complete without a scheduler round trip. */
    for(unsigned i=0;i<64;i++){
        if(!(__atomic_load_n(reg,__ATOMIC_ACQUIRE)&0x10000u))return;
#if defined(__x86_64__)
        __asm__ __volatile__("pause");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }
    __atomic_fetch_add(&waiters,1,__ATOMIC_SEQ_CST);
    uint32_t expected=__atomic_load_n(reg,__ATOMIC_ACQUIRE);
    if(expected&0x10000u){
        const struct timespec timeout={0,1000000};
        /* Atomic value check prevents a lost wake between this load and sleep.
         * Timeout/signals/spurious wakes always return to the original test. */
        syscall(SYS_futex,reg,FUTEX_WAIT_PRIVATE,expected,&timeout,NULL,0);
    }
    __atomic_fetch_sub(&waiters,1,__ATOMIC_SEQ_CST);
}
void hitz_linux_gpu_wait_notify(volatile uint32_t *reg)
{
    if(__atomic_load_n(&waiters,__ATOMIC_SEQ_CST))
        syscall(SYS_futex,reg,FUTEX_WAKE_PRIVATE,INT_MAX,NULL,NULL,0);
}
void hitz_linux_gpu_wait(uint32_t guest_va)
{
    hitz_linux_gpu_wait_hint((const volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset()+guest_va));
}
