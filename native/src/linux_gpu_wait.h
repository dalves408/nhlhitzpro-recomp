#ifndef HITZ_LINUX_GPU_WAIT_H
#define HITZ_LINUX_GPU_WAIT_H
#include <stdint.h>
void hitz_linux_gpu_wait_hint(const volatile uint32_t *reg);
void hitz_linux_gpu_wait(uint32_t guest_va);
void hitz_linux_gpu_wait_notify(volatile uint32_t *reg);
int hitz_linux_gpu_wait_cooperative(void);
void hitz_linux_gpu_wait_poll(void);
#endif
