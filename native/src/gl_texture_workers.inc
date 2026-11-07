/* JOJO item 2: render-side worker pool for texture snapshot comparisons.
 * RECOMP_GL_WORKERS=N (0 = off, default). Workers only compare memory;
 * every GL call and all cache state stay on the render thread, and the
 * render thread waits for all chunks before using the result. */
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#define CW_MAX 8
static int cw_n=-1;static size_t cw_min=262144;
static const unsigned char *cw_a,*cw_b;static size_t cw_len,cw_chunk;
static atomic_uint cw_gen,cw_left;static atomic_int cw_diff;
static uint64_t cw_jobs;
static void cw_part(int i){size_t o=cw_chunk*(size_t)i;if(o>=cw_len)return;size_t n=cw_len-o<cw_chunk?cw_len-o:cw_chunk;
    if(!atomic_load_explicit(&cw_diff,memory_order_relaxed)&&memcmp(cw_a+o,cw_b+o,n))atomic_store(&cw_diff,1);}
static void *cw_main(void *arg){int id=(int)(intptr_t)arg;unsigned seen=0;
    for(;;){unsigned g;unsigned spins=0;
        while((g=atomic_load_explicit(&cw_gen,memory_order_acquire))==seen){
            if(++spins<4096)__builtin_ia32_pause();else{struct timespec ts={0,50000};nanosleep(&ts,NULL);} }
        seen=g;cw_part(id+1);atomic_fetch_sub_explicit(&cw_left,1,memory_order_release);}
    return NULL;}
static void cw_init(void){if(cw_n>=0)return;const char *v=getenv("RECOMP_GL_WORKERS");cw_n=v?atoi(v):0;if(cw_n<0)cw_n=0;if(cw_n>CW_MAX)cw_n=CW_MAX;
    const char *m=getenv("RECOMP_GL_WORKERS_MIN");if(m&&atoi(m)>0)cw_min=(size_t)atoi(m);
    for(int i=0;i<cw_n;i++){pthread_t th;if(pthread_create(&th,NULL,cw_main,(void*)(intptr_t)i)){cw_n=i;break;}pthread_detach(th);}
    if(cw_n)fprintf(stderr,"[GL] texture compare workers=%d min=%zu\n",cw_n,cw_min);}
static int cw_memcmp(const void *a,const void *b,size_t n){cw_init();
    if(!cw_n||n<cw_min)return memcmp(a,b,n)!=0;
    cw_a=a;cw_b=b;cw_len=n;cw_chunk=(n/(size_t)(cw_n+1)+63)&~(size_t)63;atomic_store(&cw_diff,0);
    atomic_store_explicit(&cw_left,(unsigned)cw_n,memory_order_relaxed);atomic_fetch_add_explicit(&cw_gen,1,memory_order_release);
    cw_part(0);while(atomic_load_explicit(&cw_left,memory_order_acquire))__builtin_ia32_pause();
    cw_jobs++;return atomic_load(&cw_diff);}
