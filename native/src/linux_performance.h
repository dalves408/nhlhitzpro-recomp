#ifndef HITZ_LINUX_PERFORMANCE_H
#define HITZ_LINUX_PERFORMANCE_H
#include <stdint.h>
#include <stdio.h>
#define HITZ_GPU_COUNTERS_MAX 256
typedef struct {char device[48],engine[64];uint64_t client,ns;} HitzGpuCounter;
typedef struct {HitzGpuCounter value[HITZ_GPU_COUNTERS_MAX];unsigned count;} HitzGpuCounters;
/* CPU follows Windows: 100% means all online logical processors are busy. */
int hitz_linux_cpu_percent(uint64_t old_cpu,uint64_t cpu,uint64_t wall,unsigned processors);
void hitz_linux_gpu_parse(FILE *file,HitzGpuCounters *counters);
int hitz_linux_gpu_percent(const HitzGpuCounters *old,const HitzGpuCounters *current,uint64_t elapsed_ns);
void hitz_linux_performance_init(void);
void hitz_linux_performance_sample(int *cpu_percent,int *gpu_percent);
void hitz_linux_performance_shutdown(void);
#endif
