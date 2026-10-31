/* nv2a_mmio_hook_x86.c -- PROJECT code, 32-bit build only (NHL_X86). The toolkit's nv2a_mmio_hook.c decodes x86-64
 * instructions to trap GPU register access (VEH); this project does not use it (the NV2A aperture is plain memory, the
 * push buffer is executed by nv2a_pb_exec.c, and the display is the Direct3D back end), so the 32-bit build links the
 * toolkit's own no-op versions of its entry points instead. */
#include "nv2a_mmio_hook.h"

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{ (void)regs; return false; }
void nv2a_hook_pvideo_consume(unsigned bank) { (void)bank; }

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }
