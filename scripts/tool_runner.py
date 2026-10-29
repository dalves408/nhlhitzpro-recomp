"""Project-local adapter: bound Capstone CFG decoding without changing the toolkit."""
from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/xboxrecomp'))
from tools.recomp.disasm import Disassembler
ORIGINAL_CFG = Disassembler.disassemble_cfg

def iter_instructions(decoder, raw_bytes, start_va, end_va, entry):
    cursor = entry
    batch_count = 64
    while cursor < end_va:
        offset = cursor - start_va
        # x86 has a 15-byte maximum instruction length. A full window can
        # contain 64 complete instructions. Stop at invalid/truncated bytes,
        # exactly as the original Capstone generator does.
        chunk = raw_bytes[offset:min(end_va - start_va, offset + 15 * batch_count)]
        count = 0
        for insn in decoder._cs.disasm(chunk, cursor, count=batch_count):
            count += 1
            cursor = insn.address + insn.size
            yield insn
        if count < batch_count:
            return

def bounded_cfg(self, raw_bytes, start_va, end_va, entry_points,
                stop_addresses=None, stop_mnemonics=()):
    size = end_va - start_va
    if size <= 0 or size > len(raw_bytes):
        return []
    decoded = {}
    stop_addresses = set(stop_addresses or ())
    worklist = list(entry_points)
    while worklist:
        entry = worklist.pop()
        if not (start_va <= entry < end_va) or entry in decoded:
            continue
        for cs_insn in iter_instructions(self, raw_bytes, start_va, end_va, entry):
            if cs_insn.address != entry and cs_insn.address in stop_addresses:
                break
            if cs_insn.address >= end_va or cs_insn.address in decoded:
                break
            insn = self._decode_instruction(cs_insn)
            decoded[insn.address] = insn
            if (insn.is_cond_jump and insn.jump_target is not None
                    and start_va <= insn.jump_target < end_va):
                worklist.append(insn.jump_target)
            if insn.is_jump:
                if (insn.jump_target is not None
                        and start_va <= insn.jump_target < end_va):
                    worklist.append(insn.jump_target)
                break
            if insn.is_ret or insn.mnemonic in stop_mnemonics:
                break
    return [decoded[addr] for addr in sorted(decoded)]

# CPU control/debug registers have no C lvalues in this runtime. Route them
# through the same diagnostic mechanism as other unsupported instructions.
from tools.recomp.lifter import Lifter
ORIGINAL_LIFT = Lifter.lift_instruction

def checked_lift(self, insn):
    if insn.address == 0x00254141 and insn.mnemonic == "mov":
        return ['{ extern void nhl_wait_gpu_fence(uint32_t, uint32_t); nhl_wait_gpu_fence(MEM32(esi + 0x30), MEM32(ebp)); }', "goto loc_0025415F;"]
    # Stadium-animation render (sub_000874C0): edi = table[obj->index], where
    # table is *(0x36E924) with *(0x36E940) entries, built from the
    # "stadanimgeom" geometry group. That group comes up empty here (open
    # bug), the table is allocated with 0 bytes, and the unchecked read picks
    # up another allocation's data as an object pointer. Out of range reads
    # as "no object", which takes the game's own null path.
    if insn.address == 0x000874EE and insn.mnemonic == "mov":
        return ["edi = (eax < MEM32(0x0036E940u)) ? MEM32(ecx + eax * 4) : 0; /* bounds-checked stadanimgeom table */"]
    # XPP's USB host driver brackets two loads (HCCA frame number, then
    # HcFmRemaining at 0xFE80200C) with cli/sti so a USB interrupt cannot
    # land between them. The pairs at sub_002FD5xx/sub_002FD618 are the only
    # ones in the title; anywhere else cli/sti still trap.
    if insn.address in (0x002FD5EA, 0x002FD5FC, 0x002FD624, 0x002FD635) \
            and insn.mnemonic in ("cli", "sti"):
        return ["/* %s: no-op (two-load window in the USB driver) */" % insn.mnemonic]
    # Resource loader (sub_0015C2A0): it starts a read with sub_001CA200, and
    # only afterwards stores the buffer bounds the completion parses from
    # (req+0x21C, a 0xFFFFFFFF marker and the size at that address). On the
    # console the read is still in flight then. Here reads finish at once, the
    # game's stream pump runs the completion inside the call -- which parses
    # from req+0x21C == 0 (empty geometry groups, NULL meshes) and frees req --
    # and the late stores land in freed memory. Do the stores before the call
    # (eax/edi/ebx/[esi+0x218] are all final by then) and drop the late ones.
    if insn.address == 0x0015C396 and insn.mnemonic == "call":
        pre = ["{ uint32_t _hdr = MEM32(esi + 0x218) + edi - 8; /* NHL: bounds before the read */",
               "  MEM32(esi + 0x21C) = _hdr; MEM32(_hdr) = 0xFFFFFFFFu; MEM32(_hdr + 4) = ebx; }"]
        return pre + ORIGINAL_LIFT(self, insn)
    if insn.address in (0x0015C39B, 0x0015C3A1, 0x0015C3A5, 0x0015C3AB,
                        0x0015C3B1, 0x0015C3BA):
        return ["/* %s: moved before the read (req may be freed by now) */" % insn.mnemonic]
    if insn.mnemonic == "wbinvd":
        return ["{ extern void nhl_guest_wbinvd(void); nhl_guest_wbinvd(); } /* coherent host memory */"]
    if any(op.type == 'reg' and op.reg and
           len(op.reg) == 3 and op.reg[:2] in ('cr', 'dr') and op.reg[2].isdigit()
           for op in insn.operands):
        return self._unimplemented(insn, insn.mnemonic, ' (privileged CPU register)')
    result = ORIGINAL_LIFT(self, insn)
    # Retail USA XBE: D3D stores the allocated completion word at device+0x30.
    # Bind the executor before the first kickoff; never synthesize completion.
    if insn.address == 0x002531E2 and insn.mnemonic == "mov":
        result.append("{ extern void nv2a_pb_set_semaphore_target(uint32_t); nv2a_pb_set_semaphore_target(eax); }")
    return result

def main():
    Disassembler.disassemble_cfg = bounded_cfg
    Lifter.lift_instruction = checked_lift
    module = sys.argv.pop(1)
    sys.argv[0] = module
    runpy.run_module(module, run_name='__main__')

if __name__ == '__main__':
    main()
