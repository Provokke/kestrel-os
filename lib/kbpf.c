#include "kbpf.h"

int kbpf_verify(const struct kbpf_insn *prog, uint32_t len, uint32_t nfields)
{
    if (len == 0 || len > KBPF_MAX_INSNS)
        return KBPF_ERR_LEN;
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct kbpf_insn *in = &prog[pc];

        if (in->op >= KBPF_NOPS)
            return KBPF_ERR_OP;
        if (in->dst >= KBPF_NREGS || in->src >= KBPF_NREGS)
            return KBPF_ERR_REG;
        switch (in->op) {
        case KBPF_LDF:
            if (in->imm >= nfields)
                return KBPF_ERR_FIELD;
            break;
        case KBPF_RSHI:
            if (in->imm >= 32)
                return KBPF_ERR_SHIFT;
            break;
        case KBPF_JEQI: case KBPF_JNEI: case KBPF_JGTI: case KBPF_JLTI: case KBPF_JA:
            if ((uint32_t)pc + 1 + in->off >= len)
                return KBPF_ERR_JUMP;
            break;
        default:
            break;
        }
    }
    if (prog[len - 1].op != KBPF_RET && prog[len - 1].op != KBPF_RETI)
        return KBPF_ERR_NORET;
    return KBPF_OK;
}

uint32_t kbpf_run(const struct kbpf_insn *prog, uint32_t len, const uint32_t *ctx)
{
    uint32_t r[KBPF_NREGS] = {0};
    uint32_t pc = 0;
    while (pc < len) {
        const struct kbpf_insn *in = &prog[pc++];
        uint32_t *d = &r[in->dst];
        switch (in->op) {
        case KBPF_LDF: *d = ctx[in->imm]; break;
        case KBPF_MOVI: *d = in->imm; break;
        case KBPF_MOV: *d = r[in->src]; break;
        case KBPF_ADDI: *d += in->imm; break;
        case KBPF_ANDI: *d &= in->imm; break;
        case KBPF_RSHI: *d >>= in->imm; break;
        case KBPF_JEQI: if (*d == in->imm) pc += in->off; break;
        case KBPF_JNEI: if (*d != in->imm) pc += in->off; break;
        case KBPF_JGTI: if (*d > in->imm) pc += in->off; break;
        case KBPF_JLTI: if (*d < in->imm) pc += in->off; break;
        case KBPF_JA: pc += in->off; break;
        case KBPF_RETI: return in->imm;
        case KBPF_RET: return *d;
        default: return 0;
        }
    }
    return 0; /* unreachable for verified programs */
}
