/* Bridge real Linux x86-64 faults to the existing APU register model. */
#include <stdbool.h>
#include <stdint.h>
#include "apu/apu.h"
#include "../overrides/linux/mmio_x64_posix.h"
extern MCPXAPUState *g_apu_state;
static uint64_t read_apu(void *dev,uint32_t off,int size)
{
    if(size==8)return mcpx_apu_mmio_read(dev,off,4)|(mcpx_apu_mmio_read(dev,off+4,4)<<32);
    return mcpx_apu_mmio_read(dev,off,size);
}
static void write_apu(void *dev,uint32_t off,uint64_t val,int size)
{
    if(size==8){mcpx_apu_mmio_write(dev,off,(uint32_t)val,4);mcpx_apu_mmio_write(dev,off+4,val>>32,4);}
    else mcpx_apu_mmio_write(dev,off,val,size);
}
bool apu_hook_handle_mmio_posix(void *ctx,uintptr_t fault,uint32_t va,int write)
{
    (void)fault;(void)write;
    return g_apu_state && hitz_mmio_emulate(ctx,va-0xfe800000u,g_apu_state,read_apu,write_apu);
}
