/* kbpf: a tiny eBPF-inspired filter VM for tracepoints (portable).
 * Programs are verified once at attach time: bounded length, valid registers
 * and context fields, forward-only jumps that stay in bounds, and a final RET.
 * Forward-only control flow guarantees termination in <= len steps. */
#ifndef KESTREL_KBPF_H
#define KESTREL_KBPF_H
#include <stdint.h>

#define KBPF_MAX_INSNS 64
#define KBPF_NREGS 4

enum kbpf_op {
    KBPF_LDF,     /* dst = ctx[imm] */

    KBPF_MOVI,    /* dst = imm */
    KBPF_MOV,     /* dst = src */
    KBPF_ADDI,    /* dst += imm */
    KBPF_ANDI,    /* dst &= imm */
    KBPF_RSHI,    /* dst >>= imm (imm < 32) */
    KBPF_JEQI,    /* if dst == imm: pc += off */
    KBPF_JNEI,
    KBPF_JGTI,    /* unsigned > */
    KBPF_JLTI,    /* unsigned < */
    KBPF_JA,      /* pc += off */
    KBPF_RETI,    /* return imm */
    KBPF_RET,     /* return dst */
    KBPF_NOPS
};

struct kbpf_insn {
    uint8_t op;
    uint8_t dst;
    uint8_t src;
    uint8_t off; /* forward jump distance (relative to next insn) */
    uint32_t imm;
};

enum kbpf_verdict {
    KBPF_OK = 0,
    KBPF_ERR_LEN = -1,
    KBPF_ERR_OP = -2,
    KBPF_ERR_REG = -3,

    KBPF_ERR_FIELD = -4,
    KBPF_ERR_JUMP = -5,
    KBPF_ERR_NORET = -6,  
    KBPF_ERR_SHIFT = -7,
};

int kbpf_verify(const struct kbpf_insn *prog, uint32_t len, uint32_t nfields);
/* Only call on verified programs. Returns program result (0 = drop). */
uint32_t kbpf_run(const struct kbpf_insn *prog, uint32_t len, const uint32_t *ctx);

#define KBPF_I(o, d, s, of, im) ((struct kbpf_insn){(uint8_t)(o), (uint8_t)(d), (uint8_t)(s), (uint8_t)(of), (uint32_t)(im)})
#endif
