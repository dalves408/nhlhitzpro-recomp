/* Opt-in host read latency, separate from guest decode/completion work. Never
 * changes buffers, offsets, status, event/APC ordering or synchronous behavior. */
static INIT_ONCE s_io_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_io_lock;
static FILE *s_io_out;
static FILE *s_io_opens;
static LARGE_INTEGER s_io_frequency, s_io_origin;
static ULONGLONG s_io_tick;
static BOOL CALLBACK io_timing_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    const char *path = getenv("RECOMP_IO_CSV");
    (void)once;(void)parameter;(void)context;
    if(!path || !*path)return TRUE;
    s_io_out=fopen(path,"w");
    if(!s_io_out){fprintf(stderr,"[IO-TIMING] cannot open %s\n",path);return TRUE;}
    InitializeCriticalSection(&s_io_lock);QueryPerformanceFrequency(&s_io_frequency);
    QueryPerformanceCounter(&s_io_origin);s_io_tick=GetTickCount64();
    setvbuf(s_io_out,NULL,_IOFBF,65536);
    fputs("start_tick_ms,end_tick_ms,duration_ms,thread_id,guest_handle,offset,requested,delivered,status,caller,buffer\n",s_io_out);
    {char opens[600];
     if(strlen(path)<580){snprintf(opens,sizeof opens,"%s.opens.csv",path);s_io_opens=fopen(opens,"w");
      if(s_io_opens)fputs("tick_ms,guest_handle,access,disposition,guest_options,host_options,path\n",s_io_opens);}}
    return TRUE;
}
static void io_timing_note_open(uint32_t handle,const char *path,unsigned length,
                               uint32_t access,uint32_t disposition,uint32_t guest_options,uint32_t host_options)
{
    unsigned i;LARGE_INTEGER opened;double tick;
    InitOnceExecuteOnce(&s_io_once,io_timing_init,NULL,NULL);
    if(!s_io_opens)return;
    QueryPerformanceCounter(&opened);
    tick=(double)s_io_tick+1000.0*(double)(opened.QuadPart-s_io_origin.QuadPart)/(double)s_io_frequency.QuadPart;
    EnterCriticalSection(&s_io_lock);
    fprintf(s_io_opens,"%.6f,%08X,%08X,%u,%08X,%08X,\"",tick,
        handle,access,disposition,guest_options,host_options);
    for(i=0;i<length;i++){if(path[i]=='\"')fputc('\"',s_io_opens);fputc(path[i],s_io_opens);}
    fputs("\"\n",s_io_opens);fflush(s_io_opens);LeaveCriticalSection(&s_io_lock);
}
static LONGLONG io_timing_begin(void)
{
    LARGE_INTEGER now;
    InitOnceExecuteOnce(&s_io_once,io_timing_init,NULL,NULL);
    if(!s_io_out)return 0;
    QueryPerformanceCounter(&now);return now.QuadPart;
}
static void io_timing_end(LONGLONG began,uint32_t handle,int64_t offset,uint32_t requested,
                          uint32_t delivered,uint32_t status,uint32_t caller,uint32_t buffer)
{
    LARGE_INTEGER end;double start_ms,end_ms;static unsigned count;
    if(!began || !s_io_out)return;
    QueryPerformanceCounter(&end);
    start_ms=(double)s_io_tick+1000.0*(double)(began-s_io_origin.QuadPart)/(double)s_io_frequency.QuadPart;
    end_ms=(double)s_io_tick+1000.0*(double)(end.QuadPart-s_io_origin.QuadPart)/(double)s_io_frequency.QuadPart;
    EnterCriticalSection(&s_io_lock);
    fprintf(s_io_out,"%.6f,%.6f,%.6f,%lu,%08X,%lld,%u,%u,%08X,%08X,%08X\n",
        start_ms,end_ms,end_ms-start_ms,(unsigned long)GetCurrentThreadId(),handle,(long long)offset,
        requested,delivered,status,caller,buffer);
    if(!(++count%64) || requested>=262144)fflush(s_io_out);
    LeaveCriticalSection(&s_io_lock);
}
