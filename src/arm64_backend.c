#define _POSIX_C_SOURCE 200809L

#include "backend_int.h"
#include "isa.h"

#include <string.h>

enum {
   A_AC = 0,
   A_PC = 4,
   A_IR = 8,
   A_AR = 12,
   A_DR = 16,
   A_E = 20,
   A_IEN = 24,
   A_FGI = 28,
   A_INPR = 32,
   A_OUTR = 36
};

static void ld(FILE* f, int off, const char* reg) {
   mano_emit(f, "    ldr %s, [x20, #%d]\n", reg, off);
}

static void st(FILE* f, int off, const char* reg) {
   mano_emit(f, "    str %s, [x20, #%d]\n", reg, off);
}

static void zero(FILE* f, int off) {
   mano_emit(f, "    str wzr, [x20, #%d]\n", off);
}

static void sys_exit(FILE* f, int code) {
   mano_emit(f, "    mov x0, #%d\n    mov x8, #93\n    svc #0\n", code);
}

static void emit_bad_epilogue(FILE* f, int badlen) {
   mano_emit(f, "\n.Lbad:\n"
                "    mov x0, #2\n"
                "    adrp x1, .Lbadmsg\n"
                "    add x1, x1, :lo12:.Lbadmsg\n");
   mano_emit(f, "    mov x2, #%d\n", badlen);
   mano_emit(f, "    mov x8, #64\n"
                "    svc #0\n");
   sys_exit(f, 1);
   mano_emit(f, "\n.section .note.GNU-stack,\"\",%%progbits\n");
}

static void emit_src_comment(FILE* f, const ManoProgram* prog, int addr) {
   int32_t li = prog->line_of[addr];
   if (li >= 0 && (size_t)li < prog->nlines)
      mano_emit(f, "\n// %03X: %s\n", addr, prog->lines[li].text);
   else
      mano_emit(f, "\n// %03X:\n", addr);
}

/* =========================================================================
 * -O0: Literal micro-ops
 * ========================================================================= */

static void o0_incpc(FILE* f) {
   ld(f, A_PC, "w0");
   mano_emit(f, "    add w0, w0, #1\n    ubfx w0, w0, #0, #12\n");
   st(f, A_PC, "w0");
}

static void o0_fetch(FILE* f, int addr, bool indirect) {
   mano_emit(f, "\n.Lx%03X:\n", addr);
   ld(f, A_PC, "w0");
   mano_emit(f, "    ldrh w1, [x19, x0, lsl #1]\n");
   st(f, A_IR, "w1");
   mano_emit(f, "    add w0, w0, #1\n    ubfx w0, w0, #0, #12\n");
   st(f, A_PC, "w0");
   mano_emit(f, "    ubfx w1, w1, #0, #12\n");
   st(f, A_AR, "w1");
   if (indirect) {
      mano_emit(f, "    ldrh w1, [x19, x1, lsl #1]\n"
                   "    ubfx w1, w1, #0, #12\n");
      st(f, A_AR, "w1");
   }
}

static void o0_mem_op(FILE* f, const ManoInstr* ins) {
   switch (ins->op) {
   case 0:
      mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n");
      st(f, A_DR, "w2");
      ld(f, A_AC, "w0");
      mano_emit(f, "    and w0, w0, w2\n");
      st(f, A_AC, "w0");
      break;
   case 1:
      mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n");
      st(f, A_DR, "w2");
      ld(f, A_AC, "w0");
      mano_emit(f, "    add w2, w0, w2\n"
                   "    lsr w3, w2, #16\n"
                   "    cmp w3, #0\n"
                   "    cset w3, ne\n");
      st(f, A_E, "w3");
      mano_emit(f, "    uxth w2, w2\n");
      st(f, A_AC, "w2");
      break;
   case 2:
      mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n");
      st(f, A_DR, "w2");
      st(f, A_AC, "w2");
      break;
   case 3:
      ld(f, A_AC, "w0");
      mano_emit(f, "    strh w0, [x19, x1, lsl #1]\n");
      break;
   case 4:
      st(f, A_PC, "w1");
      break;
   case 5:
      ld(f, A_PC, "w0");
      mano_emit(f, "    strh w0, [x19, x1, lsl #1]\n"
                   "    add w0, w1, #1\n"
                   "    ubfx w0, w0, #0, #12\n");
      st(f, A_PC, "w0");
      break;
   case 6:
      mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                   "    add w2, w2, #1\n"
                   "    uxth w2, w2\n"
                   "    strh w2, [x19, x1, lsl #1]\n"
                   "    cbnz w2, 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   default:
      break;
   }
}

static void o0_reg_op(FILE* f, int code) {
   switch (code) {
   case 0x7800:
      zero(f, A_AC);
      break;
   case 0x7400:
      zero(f, A_E);
      break;
   case 0x7200:
      ld(f, A_AC, "w0");
      mano_emit(f, "    mvn w0, w0\n    uxth w0, w0\n");
      st(f, A_AC, "w0");
      break;
   case 0x7100:
      ld(f, A_E, "w0");
      mano_emit(f, "    eor w0, w0, #1\n");
      st(f, A_E, "w0");
      break;
   case 0x7080:
      ld(f, A_AC, "w0");
      ld(f, A_E, "w2");
      mano_emit(f, "    and w4, w0, #1\n"
                   "    lsr w0, w0, #1\n"
                   "    lsl w3, w2, #15\n"
                   "    orr w0, w0, w3\n");
      st(f, A_AC, "w0");
      st(f, A_E, "w4");
      break;
   case 0x7040:
      ld(f, A_AC, "w0");
      ld(f, A_E, "w2");
      mano_emit(f, "    lsr w3, w0, #15\n"
                   "    lsl w0, w0, #1\n"
                   "    uxth w0, w0\n"
                   "    orr w0, w0, w2\n");
      st(f, A_AC, "w0");
      st(f, A_E, "w3");
      break;
   case 0x7020:
      ld(f, A_AC, "w0");
      mano_emit(f, "    add w0, w0, #1\n    uxth w0, w0\n");
      st(f, A_AC, "w0");
      break;
   case 0x7010:
      ld(f, A_AC, "w0");
      mano_emit(f, "    tst w0, #0x8000\n    b.ne 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7008:
      ld(f, A_AC, "w0");
      mano_emit(f, "    tst w0, #0x8000\n    b.eq 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7004:
      ld(f, A_AC, "w0");
      mano_emit(f, "    cmp w0, #0\n    b.ne 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7002:
      ld(f, A_E, "w0");
      mano_emit(f, "    cbnz w0, 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7001:
      sys_exit(f, 0);
      break;
   default:
      break;
   }
}

static void o0_io_op(FILE* f, int code) {
   switch (code) {
   case 0xF800:
      ld(f, A_FGI, "w0");
      mano_emit(f, "    cbnz w0, 2f\n");
      zero(f, A_INPR);
      mano_emit(f, "    mov x0, #0\n"
                   "    add x1, x20, #32\n"
                   "    mov x2, #1\n"
                   "    mov x8, #63\n"
                   "    svc #0\n"
                   "    mov w0, #1\n");
      st(f, A_FGI, "w0");
      mano_emit(f, "2:\n");
      ld(f, A_INPR, "w0");
      st(f, A_AC, "w0");
      zero(f, A_FGI);
      break;
   case 0xF400:
      ld(f, A_AC, "w0");
      mano_emit(f, "    and w0, w0, #255\n"
                   "    str w0, [x20, #36]\n"
                   "    mov x0, #1\n"
                   "    add x1, x20, #36\n"
                   "    mov x2, #1\n"
                   "    mov x8, #64\n"
                   "    svc #0\n");
      break;
   case 0xF200:
      ld(f, A_FGI, "w0");
      mano_emit(f, "    cbnz w0, 2f\n");
      zero(f, A_INPR);
      mano_emit(f, "    mov x0, #0\n"
                   "    add x1, x20, #32\n"
                   "    mov x2, #1\n"
                   "    mov x8, #63\n"
                   "    svc #0\n"
                   "    cmp x0, #0\n"
                   "    b.le 2f\n"
                   "    mov w0, #1\n");
      st(f, A_FGI, "w0");
      mano_emit(f, "2:\n");
      ld(f, A_FGI, "w0");
      mano_emit(f, "    cbz w0, 3f\n");
      o0_incpc(f);
      mano_emit(f, "3:\n");
      break;
   case 0xF100:
      o0_incpc(f);
      break;
   case 0xF080:
      mano_emit(f, "    mov w0, #1\n    str w0, [x20, #24]\n");
      break;
   case 0xF040:
      zero(f, A_IEN);
      break;
   default:
      break;
   }
}

static void emit_arm64_o0(FILE* f, const ManoProgram* prog, int start_pc) {
   mano_emit(f, ".arch armv8-a\n");
   mano_emit_dataseg(f, prog);
   mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, "quad");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n"
                "    adrp x19, .Lmem\n"
                "    add x19, x19, :lo12:.Lmem\n"
                "    adrp x20, .Lstate\n"
                "    add x20, x20, :lo12:.Lstate\n"
                "    adrp x21, .Ljtab\n"
                "    add x21, x21, :lo12:.Ljtab\n");
   mano_emit(f, "    mov w0, #%d\n    str w0, [x20, #4]\n",
             start_pc & 0x0FFF);
   mano_emit(f, "    b .Ldispatch\n"
                "\n.Ldispatch:\n"
                "    ldr w0, [x20, #4]\n"
                "    ubfx w0, w0, #0, #12\n"
                "    ldr x1, [x21, x0, lsl #3]\n"
                "    br x1\n");

   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      if (!prog->is_code[addr])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;
      emit_src_comment(f, prog, addr);
      o0_fetch(f, addr, ins.indirect != 0);
      if (ins.kind == MANO_KIND_MEM) {
         o0_mem_op(f, &ins);
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         o0_reg_op(f, code);
         if (code == 0x7001)
            continue;
      }
      else {
         o0_io_op(f, mano_mnemonics[ins.op].code);
      }
      mano_emit(f, "    b .Ldispatch\n");
   }

   emit_bad_epilogue(f, badlen);
}

/* =========================================================================
 * -O1: Register-cached AC (w22), E (w23), PC (w24), dynamic fetch & dispatch
 * ========================================================================= */

static void emit_arm64_o1(FILE* f, const ManoProgram* prog, int start_pc) {
   mano_emit(f, ".arch armv8-a\n");
   mano_emit_dataseg(f, prog);
   mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, "quad");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n"
                "    adrp x19, .Lmem\n"
                "    add x19, x19, :lo12:.Lmem\n"
                "    adrp x20, .Lstate\n"
                "    add x20, x20, :lo12:.Lstate\n"
                "    adrp x21, .Ljtab\n"
                "    add x21, x21, :lo12:.Ljtab\n"
                "    mov w22, #0\n"
                "    mov w23, #0\n");
   mano_emit(f, "    mov w24, #%d\n"
                "\n.Ldispatch:\n"
                "    ldr x1, [x21, x24, lsl #3]\n"
                "    br x1\n",
             start_pc & 0x0FFF);

   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      if (!prog->is_code[addr])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;

      emit_src_comment(f, prog, addr);
      mano_emit(f, ".Lx%03X:\n", addr);

      if (ins.kind == MANO_KIND_MEM) {
         mano_emit(f, "    ldrh w1, [x19, x24, lsl #1]\n"
                      "    add w24, w24, #1\n"
                      "    ubfx w24, w24, #0, #12\n"
                      "    ubfx w1, w1, #0, #12\n");
         if (ins.indirect) {
            mano_emit(f, "    ldrh w1, [x19, x1, lsl #1]\n"
                         "    ubfx w1, w1, #0, #12\n");
         }
         switch (ins.op) {
         case 0:
            mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                         "    and w22, w22, w2\n");
            break;
         case 1:
            mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                         "    add w22, w22, w2\n"
                         "    lsr w23, w22, #16\n"
                         "    uxth w22, w22\n");
            break;
         case 2:
            mano_emit(f, "    ldrh w22, [x19, x1, lsl #1]\n");
            break;
         case 3:
            mano_emit(f, "    strh w22, [x19, x1, lsl #1]\n");
            break;
         case 4:
            mano_emit(f, "    mov w24, w1\n");
            break;
         case 5:
            mano_emit(f, "    strh w24, [x19, x1, lsl #1]\n"
                         "    add w24, w1, #1\n"
                         "    ubfx w24, w24, #0, #12\n");
            break;
         case 6:
            mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                         "    add w2, w2, #1\n"
                         "    uxth w2, w2\n"
                         "    strh w2, [x19, x1, lsl #1]\n"
                         "    cbnz w2, 1f\n"
                         "    add w24, w24, #1\n"
                         "    ubfx w24, w24, #0, #12\n"
                         "1:\n");
            break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0x7001) {
            sys_exit(f, 0);
            continue;
         }
         mano_emit(f, "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n");
         switch (code) {
         case 0x7800:
            mano_emit(f, "    mov w22, #0\n");
            break;
         case 0x7400:
            mano_emit(f, "    mov w23, #0\n");
            break;
         case 0x7200:
            mano_emit(f, "    mvn w22, w22\n    uxth w22, w22\n");
            break;
         case 0x7100:
            mano_emit(f, "    eor w23, w23, #1\n");
            break;
         case 0x7080:
            mano_emit(f, "    and w4, w22, #1\n"
                         "    lsr w22, w22, #1\n"
                         "    lsl w3, w23, #15\n"
                         "    orr w22, w22, w3\n"
                         "    mov w23, w4\n");
            break;
         case 0x7040:
            mano_emit(f, "    lsr w4, w22, #15\n"
                         "    lsl w22, w22, #1\n"
                         "    uxth w22, w22\n"
                         "    orr w22, w22, w23\n"
                         "    mov w23, w4\n");
            break;
         case 0x7020:
            mano_emit(f, "    add w22, w22, #1\n    uxth w22, w22\n");
            break;
         case 0x7010:
            mano_emit(f, "    tst w22, #0x8000\n    b.ne 1f\n"
                         "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n"
                         "1:\n");
            break;
         case 0x7008:
            mano_emit(f, "    tst w22, #0x8000\n    b.eq 1f\n"
                         "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n"
                         "1:\n");
            break;
         case 0x7004:
            mano_emit(f, "    cbnz w22, 1f\n"
                         "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n"
                         "1:\n");
            break;
         case 0x7002:
            mano_emit(f, "    cbnz w23, 1f\n"
                         "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n"
                         "1:\n");
            break;
         default:
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         mano_emit(f, "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n");
         switch (code) {
         case 0xF800:
            ld(f, A_FGI, "w0");
            mano_emit(f, "    cbnz w0, 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov x0, #0\n"
                         "    add x1, x20, #32\n"
                         "    mov x2, #1\n"
                         "    mov x8, #63\n"
                         "    svc #0\n"
                         "2:\n");
            ld(f, A_INPR, "w22");
            zero(f, A_FGI);
            break;
         case 0xF400:
            mano_emit(f, "    and w0, w22, #255\n"
                         "    str w0, [x20, #36]\n"
                         "    mov x0, #1\n"
                         "    add x1, x20, #36\n"
                         "    mov x2, #1\n"
                         "    mov x8, #64\n"
                         "    svc #0\n");
            break;
         case 0xF200:
            ld(f, A_FGI, "w0");
            mano_emit(f, "    cbnz w0, 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov x0, #0\n"
                         "    add x1, x20, #32\n"
                         "    mov x2, #1\n"
                         "    mov x8, #63\n"
                         "    svc #0\n"
                         "    cmp x0, #0\n"
                         "    b.le 3f\n"
                         "    mov w0, #1\n");
            st(f, A_FGI, "w0");
            mano_emit(f, "2:\n"
                         "    add w24, w24, #1\n"
                         "    ubfx w24, w24, #0, #12\n"
                         "3:\n");
            break;
         case 0xF100:
            mano_emit(f, "    add w24, w24, #1\n    ubfx w24, w24, #0, #12\n");
            break;
         case 0xF080:
         case 0xF040:
            break;
         default:
            break;
         }
      }
      mano_emit(f, "    b .Ldispatch\n");
   }

   emit_bad_epilogue(f, badlen);
}

/* =========================================================================
 * -O2 & -O3: Direct control flow, static operands, register promotion, fusion
 * ========================================================================= */

static const char* const a64_prom[MANO_MAX_PROMOTED] = {
    "w24", "w25", "w26", "w27", "w28", "w29"};

static void a64_branch(FILE* f, const ManoProgram* prog, const char* op,
                       int target) {
   target &= MANO_ADDR_MASK;
   if (prog->is_code[target])
      mano_emit(f, "    %s .Lx%03X\n", op, target);
   else
      mano_emit(f, "    %s .Lbad\n", op);
}

static void a64_cb(FILE* f, const ManoProgram* prog, const char* op,
                   const char* reg, int target) {
   target &= MANO_ADDR_MASK;
   if (prog->is_code[target])
      mano_emit(f, "    %s %s, .Lx%03X\n", op, reg, target);
   else
      mano_emit(f, "    %s %s, .Lbad\n", op, reg);
}

static void a64_fallthrough(FILE* f, const ManoProgram* prog, int next,
                            int next_emitted) {
   next &= MANO_ADDR_MASK;
   if (next == next_emitted)
      return;
   a64_branch(f, prog, "b", next);
}

static void a64_add_imm(FILE* f, const char* dst, const char* src, unsigned val) {
   if (val <= 4095) {
      mano_emit(f, "    add %s, %s, #%u\n", dst, src, val);
   }
   else {
      mano_emit(f, "    mov w2, #%u\n    add %s, %s, w2\n", val, dst, src);
   }
}

static bool a64_can_fuse(const ManoProgram* prog, const ManoAnalysis* an,
                         int opt_level, int addr, ManoInstr* next_ins) {
   if (opt_level < 3)
      return false;
   int nxt = (addr + 1) & MANO_ADDR_MASK;
   if (nxt != addr + 1 || !an->reachable[nxt] || an->is_target[nxt])
      return false;
   return mano_decode(prog->mem[nxt], next_ins);
}

static bool a64_is_direct_bun(const ManoInstr* ins, int* target) {
   if (ins->kind == MANO_KIND_MEM && ins->op == 4 && !ins->indirect) {
      *target = ins->addr & MANO_ADDR_MASK;
      return true;
   }
   return false;
}

static int a64_next_emitted(const ManoAnalysis* an, const uint8_t* skip_emit,
                            int after) {
   for (int a = after + 1; a < MANO_MEM_WORDS; a++) {
      if (an->reachable[a] && !skip_emit[a])
         return a;
   }
   return -1;
}

static void emit_arm64_opt(FILE* f, const ManoProgram* prog, int start_pc,
                           int opt_level) {
   ManoAnalysis an;
   mano_analyze(prog, start_pc, opt_level, MANO_MAX_PROMOTED, &an);

   uint8_t fused_away[MANO_MEM_WORDS];
   memset(fused_away, 0, sizeof fused_away);

   if (opt_level >= 3) {
      for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
         if (!an.reachable[addr] || fused_away[addr])
            continue;
         ManoInstr ins;
         if (!mano_decode(prog->mem[addr], &ins))
            continue;

         ManoInstr n1;
         if (ins.kind == MANO_KIND_MEM && ins.op == 6 && !ins.indirect) {
            int bun_t = 0;
            if (a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                a64_is_direct_bun(&n1, &bun_t))
               fused_away[addr + 1] = 1;
         }
         else if (ins.kind == MANO_KIND_MEM && ins.op == 2 &&
                  !ins.indirect) {
            ManoInstr n2;
            int a = ins.addr & MANO_ADDR_MASK;
            if (!an.need_e && an.slot_reg[a] >= 0 &&
                a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                n1.kind == MANO_KIND_MEM && n1.op == 1 && !n1.indirect &&
                a64_can_fuse(prog, &an, opt_level, addr + 1, &n2) &&
                n2.kind == MANO_KIND_MEM && n2.op == 3 && !n2.indirect &&
                ins.addr == n2.addr) {
               fused_away[addr + 1] = 1;
               fused_away[addr + 2] = 1;
            }
            else if (an.is_const[a] &&
                     a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                     n1.kind == MANO_KIND_MEM && !n1.indirect) {
               int a1 = n1.addr & MANO_ADDR_MASK;
               if ((!an.need_e && n1.op == 1 && an.is_const[a1]) ||
                   (n1.op == 0 && an.is_const[a1]) ||
                   (n1.op == 3 &&
                    (an.slot_reg[a1] >= 0 || an.is_dead_store[a1]))) {
                  fused_away[addr + 1] = 1;
               }
            }
         }
         else if (ins.kind == MANO_KIND_REG) {
            int code = mano_mnemonics[ins.op].code;
            if (code == 0x7010 || code == 0x7008 || code == 0x7004 ||
                code == 0x7002) {
               int bun_t = 0;
               if (a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   a64_is_direct_bun(&n1, &bun_t))
                  fused_away[addr + 1] = 1;
            }
            else if (code == 0x7800 || code == 0x7020) {
               int cur = addr;
               while (a64_can_fuse(prog, &an, opt_level, cur, &n1) &&
                      n1.kind == MANO_KIND_REG &&
                      mano_mnemonics[n1.op].code == 0x7020) {
                  fused_away[cur + 1] = 1;
                  cur++;
               }
            }
            else if (code == 0x7200) {
               if (a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   n1.kind == MANO_KIND_REG &&
                   mano_mnemonics[n1.op].code == 0x7020)
                  fused_away[addr + 1] = 1;
            }
         }
         else if (ins.kind == MANO_KIND_IO) {
            int code = mano_mnemonics[ins.op].code;
            if (code == 0xF200) {
               int bun_t = 0;
               if (a64_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   a64_is_direct_bun(&n1, &bun_t))
                  fused_away[addr + 1] = 1;
            }
         }
      }
   }

   mano_emit(f, ".arch armv8-a\n");
   if (an.need_mem)
      mano_emit_dataseg(f, prog);
   if (an.need_io_state)
      mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   if (an.need_jtab)
      mano_emit_jumptable(f, prog, "quad");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n");
   if (an.need_mem)
      mano_emit(f, "    adrp x19, .Lmem\n    add x19, x19, :lo12:.Lmem\n");
   if (an.need_io_state)
      mano_emit(f, "    adrp x20, .Lstate\n    add x20, x20, :lo12:.Lstate\n");
   if (an.need_jtab)
      mano_emit(f, "    adrp x21, .Ljtab\n    add x21, x21, :lo12:.Ljtab\n");

   mano_emit(f, "    mov w22, #0\n");
   if (an.need_e)
      mano_emit(f, "    mov w23, #0\n");
   for (int i = 0; i < an.num_promoted; i++) {
      int paddr = an.promoted_addr[i];
      uint32_t initv = an.is_counter[paddr]
                           ? (65536u - (uint32_t)prog->mem[paddr])
                           : (uint32_t)prog->mem[paddr];
      mano_emit(f, "    mov %s, #%u\n", a64_prom[i], (unsigned)initv);
   }

   int first_emitted = a64_next_emitted(&an, fused_away, -1);
   int entry = start_pc & MANO_ADDR_MASK;
   if (entry != first_emitted || !prog->is_code[entry])
      a64_branch(f, prog, "b", entry);

   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      if (!an.reachable[addr] || fused_away[addr])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;

      emit_src_comment(f, prog, addr);
      if (opt_level >= 3 && an.is_loop_header[addr])
         mano_emit(f, "    .p2align 4\n");
      if (opt_level < 3 || an.is_target[addr] || an.need_jtab)
         mano_emit(f, ".Lx%03X:\n", addr);

      int next_emitted = a64_next_emitted(&an, fused_away, addr);

      if (ins.kind == MANO_KIND_MEM) {
         int a = ins.addr & MANO_ADDR_MASK;
         int slot = (!ins.indirect) ? an.slot_reg[a] : -1;
         bool is_c = (!ins.indirect && an.is_const[a]);
         uint16_t cval = prog->mem[a];

         /* Fused LDA X; ADD Y; STA X (modular 32-bit add, no uxth in loop) */
         if (ins.op == 2 && !ins.indirect && slot >= 0 && !an.need_e &&
             addr + 2 < MANO_MEM_WORDS &&
             fused_away[addr + 1] && fused_away[addr + 2]) {
            ManoInstr add_ins;
            mano_decode(prog->mem[addr + 1], &add_ins);
            int ya = add_ins.addr & MANO_ADDR_MASK;
            const char* rx = a64_prom[slot];
            if (an.is_const[ya]) {
               uint16_t yv = prog->mem[ya];
               if (yv != 0)
                  a64_add_imm(f, rx, rx, (unsigned)yv);
            }
            else if (an.slot_reg[ya] >= 0) {
               mano_emit(f, "    add %s, %s, %s\n",
                         rx, rx, a64_prom[an.slot_reg[ya]]);
            }
            else {
               mano_emit(f, "    ldrh w2, [x19, #%d]\n"
                            "    add %s, %s, w2\n",
                         ya * 2, rx, rx);
            }
            if (an.ac_live_out[addr + 2])
               mano_emit(f, "    uxth w22, %s\n", rx);
            a64_fallthrough(f, prog, addr + 3, next_emitted);
            continue;
         }

         /* Fused LDA C; (ADD C2 | AND C2 | STA Y) */
         if (ins.op == 2 && is_c && addr + 1 < MANO_MEM_WORDS &&
             fused_away[addr + 1]) {
            ManoInstr n1;
            mano_decode(prog->mem[addr + 1], &n1);
            int a1 = n1.addr & MANO_ADDR_MASK;
            if (n1.op == 1 || n1.op == 0) {
               uint16_t v2 = prog->mem[a1];
               uint16_t res = (n1.op == 1)
                                  ? (uint16_t)((cval + v2) & 0xFFFFu)
                                  : (uint16_t)(cval & v2);
               if (an.ac_live_out[addr + 1])
                  mano_emit(f, "    mov w22, #%u\n", (unsigned)res);
            }
            else if (n1.op == 3) {
               if (!an.is_dead_store[a1]) {
                  int ys = an.slot_reg[a1];
                  uint32_t sv = an.is_counter[a1]
                                    ? (65536u - (uint32_t)cval)
                                    : (uint32_t)cval;
                  mano_emit(f, "    mov %s, #%u\n",
                            a64_prom[ys], (unsigned)sv);
               }
               if (an.ac_live_out[addr + 1])
                  mano_emit(f, "    mov w22, #%u\n", (unsigned)cval);
            }
            a64_fallthrough(f, prog, addr + 2, next_emitted);
            continue;
         }

         if (ins.indirect) {
            mano_emit(f, "    ldrh w1, [x19, #%d]\n"
                         "    ubfx w1, w1, #0, #12\n",
                      a * 2);
            switch (ins.op) {
            case 0:
               mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                            "    and w22, w22, w2\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 1:
               mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                            "    add w22, w22, w2\n");
               if (an.need_e)
                  mano_emit(f, "    lsr w23, w22, #16\n");
               mano_emit(f, "    uxth w22, w22\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 2:
               mano_emit(f, "    ldrh w22, [x19, x1, lsl #1]\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 3:
               mano_emit(f, "    strh w22, [x19, x1, lsl #1]\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 4:
               mano_emit(f, "    ldr x2, [x21, x1, lsl #3]\n    br x2\n");
               break;
            case 5:
               mano_emit(f, "    mov w0, #%d\n"
                            "    strh w0, [x19, x1, lsl #1]\n"
                            "    add w1, w1, #1\n"
                            "    ubfx w1, w1, #0, #12\n"
                            "    ldr x2, [x21, x1, lsl #3]\n"
                            "    br x2\n",
                         (addr + 1) & MANO_ADDR_MASK);
               break;
            case 6:
               mano_emit(f, "    ldrh w2, [x19, x1, lsl #1]\n"
                            "    add w2, w2, #1\n"
                            "    uxth w2, w2\n"
                            "    strh w2, [x19, x1, lsl #1]\n");
               a64_cb(f, prog, "cbz", "w2", addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            default:
               break;
            }
            continue;
         }

         switch (ins.op) {
         case 0:
            if (is_c)
               mano_emit(f, "    mov w2, #%u\n    and w22, w22, w2\n",
                         (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    and w22, w22, %s\n", a64_prom[slot]);
            else
               mano_emit(f, "    ldrh w2, [x19, #%d]\n"
                            "    and w22, w22, w2\n",
                         a * 2);
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 1:
            if (is_c)
               a64_add_imm(f, "w22", "w22", (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    add w22, w22, %s\n", a64_prom[slot]);
            else
               mano_emit(f, "    ldrh w2, [x19, #%d]\n"
                            "    add w22, w22, w2\n",
                         a * 2);
            if (an.need_e)
               mano_emit(f, "    lsr w23, w22, #16\n");
            mano_emit(f, "    uxth w22, w22\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 2:
            if (opt_level < 3 || an.ac_live_out[addr]) {
               if (is_c)
                  mano_emit(f, "    mov w22, #%u\n", (unsigned)cval);
               else if (slot >= 0)
                  mano_emit(f, "    uxth w22, %s\n", a64_prom[slot]);
               else
                  mano_emit(f, "    ldrh w22, [x19, #%d]\n", a * 2);
            }
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 3:
            if (an.is_dead_store[a]) {
               /* Write-only slot: eliminated at -O3. */
            }
            else if (an.is_counter[a]) {
               const char* rx = a64_prom[slot];
               mano_emit(f, "    mov %s, #65536\n    sub %s, %s, w22\n",
                         rx, rx, rx);
            }
            else if (slot >= 0) {
               mano_emit(f, "    mov %s, w22\n", a64_prom[slot]);
            }
            else {
               mano_emit(f, "    strh w22, [x19, #%d]\n", a * 2);
            }
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 4:
            a64_branch(f, prog, "b", a);
            break;
         case 5:
            mano_emit(f, "    mov w0, #%d\n    strh w0, [x19, #%d]\n",
                      (addr + 1) & MANO_ADDR_MASK, a * 2);
            a64_branch(f, prog, "b", a + 1);
            break;
         case 6: {
            if (an.is_counter[a]) {
               const char* reg = a64_prom[slot];
               mano_emit(f, "    subs %s, %s, #1\n", reg, reg);
               if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
                  ManoInstr bun;
                  mano_decode(prog->mem[addr + 1], &bun);
                  a64_branch(f, prog, "b.ne", bun.addr);
                  mano_emit(f, "    mov %s, #65536\n", reg);
                  a64_fallthrough(f, prog, addr + 2, next_emitted);
               }
               else {
                  mano_emit(f, "    b.ne 1f\n    mov %s, #65536\n", reg);
                  a64_branch(f, prog, "b", addr + 2);
                  mano_emit(f, "1:\n");
                  a64_fallthrough(f, prog, addr + 1, next_emitted);
               }
               break;
            }
            const char* reg = (slot >= 0) ? a64_prom[slot] : "w2";
            if (slot >= 0) {
               mano_emit(f, "    add %s, %s, #1\n    uxth %s, %s\n",
                         reg, reg, reg, reg);
            }
            else {
               mano_emit(f, "    ldrh w2, [x19, #%d]\n"
                            "    add w2, w2, #1\n"
                            "    uxth w2, w2\n"
                            "    strh w2, [x19, #%d]\n",
                         a * 2, a * 2);
            }
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a64_cb(f, prog, "cbnz", reg, bun.addr);
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a64_cb(f, prog, "cbz", reg, addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
         } break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         switch (code) {
         case 0x7800: {
            int count = 0;
            while (addr + count + 1 < MANO_MEM_WORDS &&
                   fused_away[addr + count + 1])
               count++;
            if (opt_level < 3 || an.ac_live_out[addr + count])
               mano_emit(f, "    mov w22, #%d\n", count & 0xFFFF);
            a64_fallthrough(f, prog, addr + 1 + count, next_emitted);
         } break;
         case 0x7400:
            if (an.need_e)
               mano_emit(f, "    mov w23, #0\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7200:
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               if (opt_level < 3 || an.ac_live_out[addr + 1])
                  mano_emit(f, "    neg w22, w22\n    uxth w22, w22\n");
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               if (opt_level < 3 || an.ac_live_out[addr])
                  mano_emit(f, "    mvn w22, w22\n    uxth w22, w22\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7100:
            if (an.need_e)
               mano_emit(f, "    eor w23, w23, #1\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7080:
            mano_emit(f, "    and w4, w22, #1\n"
                         "    lsr w22, w22, #1\n"
                         "    lsl w3, w23, #15\n"
                         "    orr w22, w22, w3\n"
                         "    mov w23, w4\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7040:
            mano_emit(f, "    lsr w4, w22, #15\n"
                         "    lsl w22, w22, #1\n"
                         "    uxth w22, w22\n"
                         "    orr w22, w22, w23\n"
                         "    mov w23, w4\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7020: {
            int count = 1;
            while (addr + count < MANO_MEM_WORDS &&
                   fused_away[addr + count])
               count++;
            if (opt_level < 3 || an.ac_live_out[addr + count - 1]) {
               a64_add_imm(f, "w22", "w22", (unsigned)(count & 0xFFFF));
               mano_emit(f, "    uxth w22, w22\n");
            }
            a64_fallthrough(f, prog, addr + count, next_emitted);
         } break;
         case 0x7010:
            mano_emit(f, "    tst w22, #0x8000\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a64_branch(f, prog, "b.ne", bun.addr);
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a64_branch(f, prog, "b.eq", addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7008:
            mano_emit(f, "    tst w22, #0x8000\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a64_branch(f, prog, "b.eq", bun.addr);
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a64_branch(f, prog, "b.ne", addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7004:
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a64_cb(f, prog, "cbnz", "w22", bun.addr);
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a64_cb(f, prog, "cbz", "w22", addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7002:
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a64_cb(f, prog, "cbnz", "w23", bun.addr);
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a64_cb(f, prog, "cbz", "w23", addr + 2);
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7001:
            sys_exit(f, 0);
            break;
         default:
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         switch (code) {
         case 0xF800:
            ld(f, A_FGI, "w0");
            mano_emit(f, "    cbnz w0, 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov x0, #0\n"
                         "    add x1, x20, #32\n"
                         "    mov x2, #1\n"
                         "    mov x8, #63\n"
                         "    svc #0\n"
                         "2:\n");
            ld(f, A_INPR, "w22");
            zero(f, A_FGI);
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF400:
            mano_emit(f, "    and w0, w22, #255\n"
                         "    str w0, [x20, #36]\n"
                         "    mov x0, #1\n"
                         "    add x1, x20, #36\n"
                         "    mov x2, #1\n"
                         "    mov x8, #64\n"
                         "    svc #0\n");
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF200:
            ld(f, A_FGI, "w0");
            mano_emit(f, "    cbnz w0, 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov x0, #0\n"
                         "    add x1, x20, #32\n"
                         "    mov x2, #1\n"
                         "    mov x8, #63\n"
                         "    svc #0\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               mano_emit(f, "    cmp x0, #0\n");
               a64_branch(f, prog, "b.le", bun.addr);
               mano_emit(f, "    mov w0, #1\n");
               st(f, A_FGI, "w0");
               mano_emit(f, "2:\n");
               a64_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               mano_emit(f, "    cmp x0, #0\n    b.le 3f\n"
                            "    mov w0, #1\n");
               st(f, A_FGI, "w0");
               mano_emit(f, "2:\n");
               a64_branch(f, prog, "b", addr + 2);
               mano_emit(f, "3:\n");
               a64_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0xF100:
            a64_fallthrough(f, prog, addr + 2, next_emitted);
            break;
         case 0xF080:
         case 0xF040:
            a64_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         default:
            break;
         }
      }
   }

   emit_bad_epilogue(f, badlen);
}

void mano_emit_arm64(FILE* f, const ManoProgram* prog, int start_pc,
                     int opt_level) {
   if (opt_level <= 0)
      emit_arm64_o0(f, prog, start_pc);
   else if (opt_level == 1)
      emit_arm64_o1(f, prog, start_pc);
   else
      emit_arm64_opt(f, prog, start_pc, opt_level);
}
