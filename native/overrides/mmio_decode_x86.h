/* mmio_decode_x86.h -- PROJECT code, 32-bit build only (NHL_X86).
 *
 * The toolkit's platform/mmio_decode.h decodes the faulting instruction of a trapped MMIO access from the x86-64
 * CONTEXT (Rip, Rax..R15). A 32-bit process has the 32-bit CONTEXT (Eip, Eax..Edi) and no REX prefixes, so this is the
 * same decoder, same API (mmio_emulate, mmio_read_fn, mmio_write_fn), against that. It defines MMIO_DECODE_H, so the
 * toolkit header becomes a no-op wherever this one is force-included (CMake: /FI for xbox_apu and xbox_usb), and it
 * maps the name Rip to Eip for the few places that print or advance the instruction pointer themselves. */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>
#include <windows.h>

#ifdef NHL_X86
#define Rip Eip

typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline DWORD *mmio_ctx_reg32(PCONTEXT c, int reg)
{
    switch (reg & 7) {
    case 0: return &c->Eax;  case 1: return &c->Ecx;  case 2: return &c->Edx;  case 3: return &c->Ebx;
    case 4: return &c->Esp;  case 5: return &c->Ebp;  case 6: return &c->Esi;  default: return &c->Edi;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3, rm = modrm & 7, len = 1;

    if (mod == 3) return 1;
    if (rm == 4) {                                   /* SIB */
        len += 1;
        if (mod == 0 && (ip[1] & 7) == 5) len += 4;  /* no base: disp32 */
    }
    if (mod == 0 && rm == 5) len += 4;               /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

enum { MMIO_ALU_ADD = 0, MMIO_ALU_OR = 1, MMIO_ALU_AND = 4, MMIO_ALU_SUB = 5,
       MMIO_ALU_XOR = 6, MMIO_ALU_CMP = 7, MMIO_ALU_TEST = 8 };

static inline uint64_t mmio_size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

static inline uint64_t mmio_alu(PCONTEXT ctx, int op, uint64_t a, uint64_t b, int size)
{
    uint64_t m = mmio_size_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case MMIO_ALU_ADD:
        r  = (a + b) & m;
        cf = r < a;
        of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_SUB: case MMIO_ALU_CMP:
        r  = (a - b) & m;
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_OR:  r = a | b; break;
    case MMIO_ALU_XOR: r = a ^ b; break;
    default:           r = a & b; break;
    }
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (cf)       ctx->EFlags |= 0x0001u;
    if (r == 0)   ctx->EFlags |= 0x0040u;
    if (r & sign) ctx->EFlags |= 0x0080u;
    if (of)       ctx->EFlags |= 0x0800u;
    return r;
}

/* Byte registers 4-7 are AH, CH, DH and BH (there is no REX in 32-bit mode). */
static inline uint64_t mmio_get_reg(PCONTEXT c, int reg, int size)
{
    if (size == 1 && reg >= 4 && reg < 8)
        return (*mmio_ctx_reg32(c, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg32(c, reg) & mmio_size_mask(size);
}

static inline void mmio_set_reg(PCONTEXT c, int reg, uint64_t v, int size)
{
    DWORD *d;
    if (size == 1 && reg >= 4 && reg < 8) {
        d  = mmio_ctx_reg32(c, reg - 4);
        *d = (*d & ~0xFF00u) | (DWORD)((v & 0xFF) << 8);
        return;
    }
    d = mmio_ctx_reg32(c, reg);
    if (size == 1)      *d = (*d & ~0xFFu)   | (DWORD)(v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFu) | (DWORD)(v & 0xFFFF);
    else                *d = (DWORD)v;
}

static inline uint64_t mmio_imm(const uint8_t *p, int size, int *len)
{
    if (size == 1) { *len = 1; return p[0]; }
    if (size == 2) { *len = 2; return (uint64_t)p[0] | ((uint64_t)p[1] << 8); }
    *len = 4;
    return (uint64_t)(uint32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* 1 if the instruction at ctx->Eip was serviced and Eip advanced past it. Covered as in the 64-bit decoder: MOV both
 * ways, MOVZX, the two-operand ALU forms (ADD/OR/AND/SUB/XOR/CMP), group 1 with an immediate, TEST. Anything else
 * returns 0 rather than guessing. */
static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev, mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Eip;
    int prefix = 0, has66 = 0, size, mlen, reg, ilen, alu;
    const uint8_t *op;
    uint64_t m, v, imm;

    if (!rd || !wr)
        return 0;
    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else break;
    }
    op   = ip + prefix;
    size = has66 ? 2 : 4;

    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        alu = op[0] >> 3;
        if (alu == 2 || alu == 3)
            return 0;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = (op[1] >> 3) & 7;
        m    = rd(dev, off, size);
        if (op[0] & 2) {
            v = mmio_alu(ctx, alu, mmio_get_reg(ctx, reg, size), m, size);
            if (alu != MMIO_ALU_CMP) mmio_set_reg(ctx, reg, v, size);
        } else {
            v = mmio_alu(ctx, alu, m, mmio_get_reg(ctx, reg, size), size);
            if (alu != MMIO_ALU_CMP) wr(dev, off, v, size);
        }
        ctx->Eip += prefix + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x88: case 0x89:
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = (op[1] >> 3) & 7;
        wr(dev, off, mmio_get_reg(ctx, reg, size), size);
        ctx->Eip += prefix + 1 + mlen;
        return 1;

    case 0xC6: case 0xC7:
        if (op[0] == 0xC6) size = 1;
        mlen = mmio_modrm_len(op + 1);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        wr(dev, off, imm & mmio_size_mask(size), size);
        ctx->Eip += prefix + 1 + mlen + ilen;
        return 1;

    case 0x8A: case 0x8B:
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = (op[1] >> 3) & 7;
        mmio_set_reg(ctx, reg, rd(dev, off, size), size);
        ctx->Eip += prefix + 1 + mlen;
        return 1;

    case 0x84: case 0x85:
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = (op[1] >> 3) & 7;
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size), mmio_get_reg(ctx, reg, size), size);
        ctx->Eip += prefix + 1 + mlen;
        return 1;

    case 0x80: case 0x81: case 0x83:
        alu = (op[1] >> 3) & 7;
        if (alu == 2 || alu == 3)
            return 0;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1);
        if (op[0] == 0x83) {
            imm  = (uint64_t)(uint32_t)(int32_t)(int8_t)op[1 + mlen];
            ilen = 1;
        } else {
            imm = mmio_imm(op + 1 + mlen, size, &ilen);
        }
        v = mmio_alu(ctx, alu, rd(dev, off, size), imm, size);
        if (alu != MMIO_ALU_CMP) wr(dev, off, v, size);
        ctx->Eip += prefix + 1 + mlen + ilen;
        return 1;

    case 0xF6: case 0xF7:
        if (((op[1] >> 3) & 7) > 1)
            return 0;
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, size), imm, size);
        ctx->Eip += prefix + 1 + mlen + ilen;
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r, r/m8|16 */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2);
            reg   = (op[2] >> 3) & 7;
            mmio_set_reg(ctx, reg, rd(dev, off, s) & mmio_size_mask(s), has66 ? 2 : 4);
            ctx->Eip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#endif /* NHL_X86 */
#endif /* MMIO_DECODE_H */
