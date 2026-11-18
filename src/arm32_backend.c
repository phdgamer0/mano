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
   mano_emit(f, "    ldr %s, [r5, #%d]\n", reg, off);
}

static void st(FILE* f, int off, const char* reg) {
   mano_emit(f, "    str %s, [r5, #%d]\n", reg, off);
}

static void zero(FILE* f, int off) {
   mano_emit(f, "    mov r12, #0\n    str r12, [r5, #%d]\n", off);
}

static void sys_exit(FILE* f, int code) {
   mano_emit(f, "    mov r0, #%d\n    mov r7, #1\n    svc #0\n", code);
}

static void emit_bad_epilogue(FILE* f, int badlen) {
   mano_emit(f, "\n.Lbad:\n"
                "    mov r0, #2\n"
                "    ldr r1, =.Lbadmsg\n");
   mano_emit(f, "    mov r2, #%d\n", badlen);
   mano_emit(f, "    mov r7, #4\n"
                "    svc #0\n");
   sys_exit(f, 1);
   mano_emit(f, "    .ltorg\n");
   mano_emit(f, "\n.section .note.GNU-stack,\"\",%%progbits\n");
}

static void emit_src_comment(FILE* f, const ManoProgram* prog, int addr) {
   int32_t li = prog->line_of[addr];
   if (li >= 0 && (size_t)li < prog->nlines)
      mano_emit(f, "\n@ %03X: %s\n", addr, prog->lines[li].text);
   else
      mano_emit(f, "\n@ %03X:\n", addr);
}

/* =========================================================================
 * -O0: Literal micro-ops
 * ========================================================================= */

static void o0_incpc(FILE* f) {
   ld(f, A_PC, "r0");
   mano_emit(f, "    add r0, r0, #1\n    ubfx r0, r0, #0, #12\n");
   st(f, A_PC, "r0");
}

static void o0_fetch(FILE* f, int addr, bool indirect) {
   mano_emit(f, "\n.Lx%03X:\n", addr);
   ld(f, A_PC, "r0");
   mano_emit(f, "    add r12, r4, r0, lsl #1\n    ldrh r1, [r12]\n");
   st(f, A_IR, "r1");
   mano_emit(f, "    add r0, r0, #1\n    ubfx r0, r0, #0, #12\n");
   st(f, A_PC, "r0");
   mano_emit(f, "    ubfx r1, r1, #0, #12\n");
   st(f, A_AR, "r1");
   if (indirect) {
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    ldrh r1, [r12]\n"
                   "    ubfx r1, r1, #0, #12\n");
      st(f, A_AR, "r1");
   }
}

static void o0_mem_op(FILE* f, const ManoInstr* ins) {
   switch (ins->op) {
   case 0:
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    ldrh r2, [r12]\n");
      st(f, A_DR, "r2");
      ld(f, A_AC, "r0");
      mano_emit(f, "    and r0, r0, r2\n");
      st(f, A_AC, "r0");
      break;
   case 1:
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    ldrh r2, [r12]\n");
      st(f, A_DR, "r2");
      ld(f, A_AC, "r0");
      mano_emit(f,
                "    add r2, r0, r2\n"
                "    lsr r3, r2, #16\n"
                "    cmp r3, #0\n"
                "    movne r3, #1\n"
                "    moveq r3, #0\n");
      st(f, A_E, "r3");
      mano_emit(f, "    uxth r2, r2\n");
      st(f, A_AC, "r2");
      break;
   case 2:
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    ldrh r2, [r12]\n");
      st(f, A_DR, "r2");
      st(f, A_AC, "r2");
      break;
   case 3:
      ld(f, A_AC, "r0");
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    strh r0, [r12]\n");
      break;
   case 4:
      st(f, A_PC, "r1");
      break;
   case 5:
      ld(f, A_PC, "r0");
      mano_emit(f, "    add r12, r4, r1, lsl #1\n    strh r0, [r12]\n"
                   "    add r0, r1, #1\n"
                   "    ubfx r0, r0, #0, #12\n");
      st(f, A_PC, "r0");
      break;
   case 6:
      mano_emit(f,
                "    add r12, r4, r1, lsl #1\n    ldrh r2, [r12]\n"
                "    add r2, r2, #1\n"
                "    uxth r2, r2\n"
                "    add r12, r4, r1, lsl #1\n    strh r2, [r12]\n"
                "    cmp r2, #0\n"
                "    bne 1f\n");
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
      ld(f, A_AC, "r0");
      mano_emit(f, "    mvn r0, r0\n    uxth r0, r0\n");
      st(f, A_AC, "r0");
      break;
   case 0x7100:
      ld(f, A_E, "r0");
      mano_emit(f, "    eor r0, r0, #1\n");
      st(f, A_E, "r0");
      break;
   case 0x7080:
      ld(f, A_AC, "r0");
      ld(f, A_E, "r2");
      mano_emit(f,
                "    and r3, r0, #1\n"
                "    str r3, [r5, #%d]\n"
                "    lsr r0, r0, #1\n"
                "    lsl r3, r2, #15\n"
                "    orr r0, r0, r3\n",
                A_E);
      st(f, A_AC, "r0");
      break;
   case 0x7040:
      ld(f, A_AC, "r0");
      ld(f, A_E, "r2");
      mano_emit(f,
                "    lsr r3, r0, #15\n"
                "    str r3, [r5, #%d]\n"
                "    lsl r0, r0, #1\n"
                "    uxth r0, r0\n"
                "    orr r0, r0, r2\n",
                A_E);
      st(f, A_AC, "r0");
      break;
   case 0x7020:
      ld(f, A_AC, "r0");
      mano_emit(f, "    add r0, r0, #1\n    uxth r0, r0\n");
      st(f, A_AC, "r0");
      break;
   case 0x7010:
      ld(f, A_AC, "r0");
      mano_emit(f, "    tst r0, #0x8000\n    bne 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7008:
      ld(f, A_AC, "r0");
      mano_emit(f, "    tst r0, #0x8000\n    beq 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7004:
      ld(f, A_AC, "r0");
      mano_emit(f, "    cmp r0, #0\n    bne 1f\n");
      o0_incpc(f);
      mano_emit(f, "1:\n");
      break;
   case 0x7002:
      ld(f, A_E, "r0");
      mano_emit(f, "    cmp r0, #0\n    bne 1f\n");
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
      ld(f, A_FGI, "r0");
      mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
      zero(f, A_INPR);
      mano_emit(f,
                "    mov r0, #0\n"
                "    add r1, r5, #32\n"
                "    mov r2, #1\n"
                "    mov r7, #3\n"
                "    svc #0\n"
                "    mov r0, #1\n");
      st(f, A_FGI, "r0");
      mano_emit(f, "2:\n");
      ld(f, A_INPR, "r0");
      st(f, A_AC, "r0");
      zero(f, A_FGI);
      break;
   case 0xF400:
      ld(f, A_AC, "r0");
      mano_emit(f,
                "    and r0, r0, #255\n"
                "    str r0, [r5, #36]\n"
                "    mov r0, #1\n"
                "    add r1, r5, #36\n"
                "    mov r2, #1\n"
                "    mov r7, #4\n"
                "    svc #0\n");
      break;
   case 0xF200:
      ld(f, A_FGI, "r0");
      mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
      zero(f, A_INPR);
      mano_emit(f,
                "    mov r0, #0\n"
                "    add r1, r5, #32\n"
                "    mov r2, #1\n"
                "    mov r7, #3\n"
                "    svc #0\n"
                "    cmp r0, #0\n"
                "    ble 2f\n"
                "    mov r0, #1\n");
      st(f, A_FGI, "r0");
      mano_emit(f, "2:\n");
      ld(f, A_FGI, "r0");
      mano_emit(f, "    cmp r0, #0\n    beq 3f\n");
      o0_incpc(f);
      mano_emit(f, "3:\n");
      break;
   case 0xF100:
      o0_incpc(f);
      break;
   case 0xF080:
      mano_emit(f, "    mov r0, #1\n    str r0, [r5, #24]\n");
      break;
   case 0xF040:
      zero(f, A_IEN);
      break;
   default:
      break;
   }
}

static void emit_arm32_o0(FILE* f, const ManoProgram* prog, int start_pc) {
   mano_emit(f, ".syntax unified\n.arch armv7-a\n.arm\n");
   mano_emit_dataseg(f, prog);
   mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, "word");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n"
                "    ldr r4, =.Lmem\n"
                "    ldr r5, =.Lstate\n"
                "    ldr r6, =.Ljtab\n");
   mano_emit(f, "    movw r0, #%d\n    str r0, [r5, #4]\n",
             start_pc & 0x0FFF);
   mano_emit(f, "    b .Ldispatch\n"
                "\n.Ldispatch:\n"
                "    ldr r0, [r5, #4]\n"
                "    ubfx r0, r0, #0, #12\n"
                "    ldr r1, [r6, r0, lsl #2]\n"
                "    bx r1\n"
                "    .ltorg\n");

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
 * -O1: Register-cached AC (r8), E (r9), PC (r10), dynamic fetch & dispatch
 * ========================================================================= */

static void emit_arm32_o1(FILE* f, const ManoProgram* prog, int start_pc) {
   mano_emit(f, ".syntax unified\n.arch armv7-a\n.arm\n");
   mano_emit_dataseg(f, prog);
   mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, "word");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n"
                "    ldr r4, =.Lmem\n"
                "    ldr r5, =.Lstate\n"
                "    ldr r6, =.Ljtab\n"
                "    mov r8, #0\n"
                "    mov r9, #0\n");
   mano_emit(f, "    movw r10, #%d\n"
                "\n.Ldispatch:\n"
                "    ldr r1, [r6, r10, lsl #2]\n"
                "    bx r1\n"
                "    .ltorg\n",
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
         mano_emit(f, "    add r12, r4, r10, lsl #1\n"
                      "    ldrh r1, [r12]\n"
                      "    add r10, r10, #1\n"
                      "    ubfx r10, r10, #0, #12\n"
                      "    ubfx r1, r1, #0, #12\n");
         if (ins.indirect) {
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    ldrh r1, [r12]\n"
                         "    ubfx r1, r1, #0, #12\n");
         }
         switch (ins.op) {
         case 0:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    ldrh r2, [r12]\n"
                         "    and r8, r8, r2\n");
            break;
         case 1:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    ldrh r2, [r12]\n"
                         "    add r8, r8, r2\n"
                         "    lsr r9, r8, #16\n"
                         "    uxth r8, r8\n");
            break;
         case 2:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    ldrh r8, [r12]\n");
            break;
         case 3:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    strh r8, [r12]\n");
            break;
         case 4:
            mano_emit(f, "    mov r10, r1\n");
            break;
         case 5:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    strh r10, [r12]\n"
                         "    add r10, r1, #1\n"
                         "    ubfx r10, r10, #0, #12\n");
            break;
         case 6:
            mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                         "    ldrh r2, [r12]\n"
                         "    add r2, r2, #1\n"
                         "    uxth r2, r2\n"
                         "    strh r2, [r12]\n"
                         "    cmp r2, #0\n"
                         "    bne 1f\n"
                         "    add r10, r10, #1\n"
                         "    ubfx r10, r10, #0, #12\n"
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
         mano_emit(f, "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n");
         switch (code) {
         case 0x7800:
            mano_emit(f, "    mov r8, #0\n");
            break;
         case 0x7400:
            mano_emit(f, "    mov r9, #0\n");
            break;
         case 0x7200:
            mano_emit(f, "    mvn r8, r8\n    uxth r8, r8\n");
            break;
         case 0x7100:
            mano_emit(f, "    eor r9, r9, #1\n");
            break;
         case 0x7080:
            mano_emit(f, "    and r3, r8, #1\n"
                         "    lsr r8, r8, #1\n"
                         "    lsl r2, r9, #15\n"
                         "    orr r8, r8, r2\n"
                         "    mov r9, r3\n");
            break;
         case 0x7040:
            mano_emit(f, "    lsr r3, r8, #15\n"
                         "    lsl r8, r8, #1\n"
                         "    uxth r8, r8\n"
                         "    orr r8, r8, r9\n"
                         "    mov r9, r3\n");
            break;
         case 0x7020:
            mano_emit(f, "    add r8, r8, #1\n    uxth r8, r8\n");
            break;
         case 0x7010:
            mano_emit(f, "    tst r8, #0x8000\n    bne 1f\n"
                         "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n"
                         "1:\n");
            break;
         case 0x7008:
            mano_emit(f, "    tst r8, #0x8000\n    beq 1f\n"
                         "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n"
                         "1:\n");
            break;
         case 0x7004:
            mano_emit(f, "    cmp r8, #0\n    bne 1f\n"
                         "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n"
                         "1:\n");
            break;
         case 0x7002:
            mano_emit(f, "    cmp r9, #0\n    bne 1f\n"
                         "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n"
                         "1:\n");
            break;
         default:
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         mano_emit(f, "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n");
         switch (code) {
         case 0xF800:
            ld(f, A_FGI, "r0");
            mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov r0, #0\n"
                         "    add r1, r5, #32\n"
                         "    mov r2, #1\n"
                         "    mov r7, #3\n"
                         "    svc #0\n"
                         "2:\n");
            ld(f, A_INPR, "r8");
            zero(f, A_FGI);
            break;
         case 0xF400:
            mano_emit(f, "    and r0, r8, #255\n"
                         "    str r0, [r5, #36]\n"
                         "    mov r0, #1\n"
                         "    add r1, r5, #36\n"
                         "    mov r2, #1\n"
                         "    mov r7, #4\n"
                         "    svc #0\n");
            break;
         case 0xF200:
            ld(f, A_FGI, "r0");
            mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov r0, #0\n"
                         "    add r1, r5, #32\n"
                         "    mov r2, #1\n"
                         "    mov r7, #3\n"
                         "    svc #0\n"
                         "    cmp r0, #0\n"
                         "    ble 3f\n"
                         "    mov r0, #1\n");
            st(f, A_FGI, "r0");
            mano_emit(f, "2:\n"
                         "    add r10, r10, #1\n"
                         "    ubfx r10, r10, #0, #12\n"
                         "3:\n");
            break;
         case 0xF100:
            mano_emit(f, "    add r10, r10, #1\n    ubfx r10, r10, #0, #12\n");
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

static const char* const a32_prom[2] = {"r10", "r11"};

static void a32_branch(FILE* f, const ManoProgram* prog, const char* op,
                       int target) {
   target &= MANO_ADDR_MASK;
   if (prog->is_code[target])
      mano_emit(f, "    %s .Lx%03X\n", op, target);
   else
      mano_emit(f, "    %s .Lbad\n", op);
}

static void a32_fallthrough(FILE* f, const ManoProgram* prog, int next,
                            int next_emitted) {
   next &= MANO_ADDR_MASK;
   if (next == next_emitted)
      return;
   a32_branch(f, prog, "b", next);
}

static void a32_mov_imm(FILE* f, const char* dst, unsigned val) {
   if (val <= 255 || val == 65536u)
      mano_emit(f, "    mov %s, #%u\n", dst, val);
   else
      mano_emit(f, "    movw %s, #%u\n", dst, val);
}

static void a32_add_imm(FILE* f, const char* dst, const char* src, unsigned val) {
   if (val <= 255) {
      mano_emit(f, "    add %s, %s, #%u\n", dst, src, val);
   }
   else {
      mano_emit(f, "    movw r2, #%u\n    add %s, %s, r2\n", val, dst, src);
   }
}

static void a32_and_imm(FILE* f, const char* dst, const char* src, unsigned val) {
   if (val <= 255) {
      mano_emit(f, "    and %s, %s, #%u\n", dst, src, val);
   }
   else {
      mano_emit(f, "    movw r2, #%u\n    and %s, %s, r2\n", val, dst, src);
   }
}

static void a32_ldrh_addr(FILE* f, const char* dst, int addr) {
   int off = (addr & MANO_ADDR_MASK) * 2;
   if (off <= 255) {
      mano_emit(f, "    ldrh %s, [r4, #%d]\n", dst, off);
   }
   else {
      mano_emit(f, "    movw r12, #%d\n    ldrh %s, [r4, r12]\n", off, dst);
   }
}

static void a32_strh_addr(FILE* f, const char* src, int addr) {
   int off = (addr & MANO_ADDR_MASK) * 2;
   if (off <= 255) {
      mano_emit(f, "    strh %s, [r4, #%d]\n", src, off);
   }
   else {
      mano_emit(f, "    movw r12, #%d\n    strh %s, [r4, r12]\n", off, src);
   }
}

static bool a32_can_fuse(const ManoProgram* prog, const ManoAnalysis* an,
                         int opt_level, int addr, ManoInstr* next_ins) {
   if (opt_level < 3)
      return false;
   int nxt = (addr + 1) & MANO_ADDR_MASK;
   if (nxt != addr + 1 || !an->reachable[nxt] || an->is_target[nxt])
      return false;
   return mano_decode(prog->mem[nxt], next_ins);
}

static bool a32_is_direct_bun(const ManoInstr* ins, int* target) {
   if (ins->kind == MANO_KIND_MEM && ins->op == 4 && !ins->indirect) {
      *target = ins->addr & MANO_ADDR_MASK;
      return true;
   }
   return false;
}

static int a32_next_emitted(const ManoAnalysis* an, const uint8_t* skip_emit,
                            int after) {
   for (int a = after + 1; a < MANO_MEM_WORDS; a++) {
      if (an->reachable[a] && !skip_emit[a])
         return a;
   }
   return -1;
}

static void emit_arm32_opt(FILE* f, const ManoProgram* prog, int start_pc,
                           int opt_level) {
   ManoAnalysis an;
   mano_analyze(prog, start_pc, opt_level, 2, &an);

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
            if (a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
                a32_is_direct_bun(&n1, &bun_t))
               fused_away[addr + 1] = 1;
         }
         else if (ins.kind == MANO_KIND_MEM && ins.op == 2 &&
                  !ins.indirect) {
            ManoInstr n2;
            int a = ins.addr & MANO_ADDR_MASK;
            if (!an.need_e && an.slot_reg[a] >= 0 &&
                a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
                n1.kind == MANO_KIND_MEM && n1.op == 1 && !n1.indirect &&
                a32_can_fuse(prog, &an, opt_level, addr + 1, &n2) &&
                n2.kind == MANO_KIND_MEM && n2.op == 3 && !n2.indirect &&
                ins.addr == n2.addr) {
               fused_away[addr + 1] = 1;
               fused_away[addr + 2] = 1;
            }
            else if (an.is_const[a] &&
                     a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
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
               if (a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   a32_is_direct_bun(&n1, &bun_t))
                  fused_away[addr + 1] = 1;
            }
            else if (code == 0x7800 || code == 0x7020) {
               int cur = addr;
               while (a32_can_fuse(prog, &an, opt_level, cur, &n1) &&
                      n1.kind == MANO_KIND_REG &&
                      mano_mnemonics[n1.op].code == 0x7020) {
                  fused_away[cur + 1] = 1;
                  cur++;
               }
            }
            else if (code == 0x7200) {
               if (a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   n1.kind == MANO_KIND_REG &&
                   mano_mnemonics[n1.op].code == 0x7020)
                  fused_away[addr + 1] = 1;
            }
         }
         else if (ins.kind == MANO_KIND_IO) {
            int code = mano_mnemonics[ins.op].code;
            if (code == 0xF200) {
               int bun_t = 0;
               if (a32_can_fuse(prog, &an, opt_level, addr, &n1) &&
                   a32_is_direct_bun(&n1, &bun_t))
                  fused_away[addr + 1] = 1;
            }
         }
      }
   }

   mano_emit(f, ".syntax unified\n.arch armv7-a\n.arm\n");
   if (an.need_mem)
      mano_emit_dataseg(f, prog);
   if (an.need_io_state)
      mano_emit(f, "\n.section .bss\n.balign 16\n.Lstate:\n    .space 40\n");

   int badlen = mano_emit_badmsg(f);
   if (an.need_jtab)
      mano_emit_jumptable(f, prog, "word");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, %%function\n"
                "_start:\n");
   if (an.need_mem)
      mano_emit(f, "    ldr r4, =.Lmem\n");
   if (an.need_io_state)
      mano_emit(f, "    ldr r5, =.Lstate\n");
   if (an.need_jtab)
      mano_emit(f, "    ldr r6, =.Ljtab\n");
   mano_emit(f, "    mov r8, #0\n");
   if (an.need_e)
      mano_emit(f, "    mov r9, #0\n");
   for (int i = 0; i < an.num_promoted; i++) {
      int paddr = an.promoted_addr[i];
      uint32_t initv = an.is_counter[paddr]
                           ? (65536u - (uint32_t)prog->mem[paddr])
                           : (uint32_t)prog->mem[paddr];
      a32_mov_imm(f, a32_prom[i], (unsigned)initv);
   }

   /* Always branch over the literal pool so ldr r4/r5/r6 stay in range. */
   a32_branch(f, prog, "b", start_pc & MANO_ADDR_MASK);
   mano_emit(f, "    .ltorg\n");

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

      int next_emitted = a32_next_emitted(&an, fused_away, addr);

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
            const char* rx = a32_prom[slot];
            if (an.is_const[ya]) {
               uint16_t yv = prog->mem[ya];
               if (yv != 0)
                  a32_add_imm(f, rx, rx, (unsigned)yv);
            }
            else if (an.slot_reg[ya] >= 0) {
               mano_emit(f, "    add %s, %s, %s\n",
                         rx, rx, a32_prom[an.slot_reg[ya]]);
            }
            else {
               a32_ldrh_addr(f, "r2", ya);
               mano_emit(f, "    add %s, %s, r2\n", rx, rx);
            }
            if (an.ac_live_out[addr + 2])
               mano_emit(f, "    uxth r8, %s\n", rx);
            a32_fallthrough(f, prog, addr + 3, next_emitted);
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
                  a32_mov_imm(f, "r8", (unsigned)res);
            }
            else if (n1.op == 3) {
               if (!an.is_dead_store[a1]) {
                  int ys = an.slot_reg[a1];
                  uint32_t sv = an.is_counter[a1]
                                    ? (65536u - (uint32_t)cval)
                                    : (uint32_t)cval;
                  a32_mov_imm(f, a32_prom[ys], (unsigned)sv);
               }
               if (an.ac_live_out[addr + 1])
                  a32_mov_imm(f, "r8", (unsigned)cval);
            }
            a32_fallthrough(f, prog, addr + 2, next_emitted);
            continue;
         }

         if (ins.indirect) {
            a32_ldrh_addr(f, "r1", a);
            mano_emit(f, "    ubfx r1, r1, #0, #12\n");
            switch (ins.op) {
            case 0:
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    ldrh r2, [r12]\n"
                            "    and r8, r8, r2\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 1:
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    ldrh r2, [r12]\n"
                            "    add r8, r8, r2\n");
               if (an.need_e)
                  mano_emit(f, "    lsr r9, r8, #16\n");
               mano_emit(f, "    uxth r8, r8\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 2:
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    ldrh r8, [r12]\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 3:
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    strh r8, [r12]\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 4:
               mano_emit(f, "    ldr r2, [r6, r1, lsl #2]\n    bx r2\n");
               break;
            case 5:
               a32_mov_imm(f, "r0", (unsigned)((addr + 1) & MANO_ADDR_MASK));
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    strh r0, [r12]\n"
                            "    add r1, r1, #1\n"
                            "    ubfx r1, r1, #0, #12\n"
                            "    ldr r2, [r6, r1, lsl #2]\n"
                            "    bx r2\n");
               break;
            case 6:
               mano_emit(f, "    add r12, r4, r1, lsl #1\n"
                            "    ldrh r2, [r12]\n"
                            "    add r2, r2, #1\n"
                            "    uxth r2, r2\n"
                            "    strh r2, [r12]\n"
                            "    cmp r2, #0\n");
               a32_branch(f, prog, "beq", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            default:
               break;
            }
            continue;
         }

         switch (ins.op) {
         case 0:
            if (is_c)
               a32_and_imm(f, "r8", "r8", (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    and r8, r8, %s\n", a32_prom[slot]);
            else {
               a32_ldrh_addr(f, "r2", a);
               mano_emit(f, "    and r8, r8, r2\n");
            }
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 1:
            if (is_c)
               a32_add_imm(f, "r8", "r8", (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    add r8, r8, %s\n", a32_prom[slot]);
            else {
               a32_ldrh_addr(f, "r2", a);
               mano_emit(f, "    add r8, r8, r2\n");
            }
            if (an.need_e)
               mano_emit(f, "    lsr r9, r8, #16\n");
            mano_emit(f, "    uxth r8, r8\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 2:
            if (opt_level < 3 || an.ac_live_out[addr]) {
               if (is_c)
                  a32_mov_imm(f, "r8", (unsigned)cval);
               else if (slot >= 0)
                  mano_emit(f, "    uxth r8, %s\n", a32_prom[slot]);
               else
                  a32_ldrh_addr(f, "r8", a);
            }
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 3:
            if (an.is_dead_store[a]) {
               /* Write-only slot: eliminated at -O3. */
            }
            else if (an.is_counter[a]) {
               const char* rx = a32_prom[slot];
               mano_emit(f, "    mov %s, #65536\n    sub %s, %s, r8\n",
                         rx, rx, rx);
            }
            else if (slot >= 0) {
               mano_emit(f, "    mov %s, r8\n", a32_prom[slot]);
            }
            else {
               a32_strh_addr(f, "r8", a);
            }
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 4:
            a32_branch(f, prog, "b", a);
            break;
         case 5:
            a32_mov_imm(f, "r0", (unsigned)((addr + 1) & MANO_ADDR_MASK));
            a32_strh_addr(f, "r0", a);
            a32_branch(f, prog, "b", a + 1);
            break;
         case 6: {
            if (an.is_counter[a]) {
               const char* reg = a32_prom[slot];
               mano_emit(f, "    subs %s, %s, #1\n", reg, reg);
               if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
                  ManoInstr bun;
                  mano_decode(prog->mem[addr + 1], &bun);
                  a32_branch(f, prog, "bne", bun.addr);
                  mano_emit(f, "    mov %s, #65536\n", reg);
                  a32_fallthrough(f, prog, addr + 2, next_emitted);
               }
               else {
                  mano_emit(f, "    bne 1f\n    mov %s, #65536\n", reg);
                  a32_branch(f, prog, "b", addr + 2);
                  mano_emit(f, "1:\n");
                  a32_fallthrough(f, prog, addr + 1, next_emitted);
               }
               break;
            }
            const char* reg = (slot >= 0) ? a32_prom[slot] : "r2";
            if (slot >= 0) {
               mano_emit(f, "    add %s, %s, #1\n"
                            "    uxth %s, %s\n"
                            "    cmp %s, #0\n",
                         reg, reg, reg, reg, reg);
            }
            else {
               a32_ldrh_addr(f, "r2", a);
               mano_emit(f, "    add r2, r2, #1\n    uxth r2, r2\n");
               a32_strh_addr(f, "r2", a);
               mano_emit(f, "    cmp r2, #0\n");
            }
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a32_branch(f, prog, "bne", bun.addr);
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a32_branch(f, prog, "beq", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
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
               a32_mov_imm(f, "r8", (unsigned)(count & 0xFFFF));
            a32_fallthrough(f, prog, addr + 1 + count, next_emitted);
         } break;
         case 0x7400:
            if (an.need_e)
               mano_emit(f, "    mov r9, #0\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7200:
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               if (opt_level < 3 || an.ac_live_out[addr + 1])
                  mano_emit(f, "    rsb r8, r8, #0\n    uxth r8, r8\n");
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               if (opt_level < 3 || an.ac_live_out[addr])
                  mano_emit(f, "    mvn r8, r8\n    uxth r8, r8\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7100:
            if (an.need_e)
               mano_emit(f, "    eor r9, r9, #1\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7080:
            mano_emit(f, "    and r3, r8, #1\n"
                         "    lsr r8, r8, #1\n"
                         "    lsl r2, r9, #15\n"
                         "    orr r8, r8, r2\n"
                         "    mov r9, r3\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7040:
            mano_emit(f, "    lsr r3, r8, #15\n"
                         "    lsl r8, r8, #1\n"
                         "    uxth r8, r8\n"
                         "    orr r8, r8, r9\n"
                         "    mov r9, r3\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7020: {
            int count = 1;
            while (addr + count < MANO_MEM_WORDS &&
                   fused_away[addr + count])
               count++;
            if (opt_level < 3 || an.ac_live_out[addr + count - 1]) {
               a32_add_imm(f, "r8", "r8", (unsigned)(count & 0xFFFF));
               mano_emit(f, "    uxth r8, r8\n");
            }
            a32_fallthrough(f, prog, addr + count, next_emitted);
         } break;
         case 0x7010:
            mano_emit(f, "    tst r8, #0x8000\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a32_branch(f, prog, "bne", bun.addr);
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a32_branch(f, prog, "beq", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7008:
            mano_emit(f, "    tst r8, #0x8000\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a32_branch(f, prog, "beq", bun.addr);
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a32_branch(f, prog, "bne", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7004:
            mano_emit(f, "    cmp r8, #0\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a32_branch(f, prog, "bne", bun.addr);
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a32_branch(f, prog, "beq", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7002:
            mano_emit(f, "    cmp r9, #0\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               a32_branch(f, prog, "bne", bun.addr);
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               a32_branch(f, prog, "beq", addr + 2);
               a32_fallthrough(f, prog, addr + 1, next_emitted);
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
            ld(f, A_FGI, "r0");
            mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov r0, #0\n"
                         "    add r1, r5, #32\n"
                         "    mov r2, #1\n"
                         "    mov r7, #3\n"
                         "    svc #0\n"
                         "2:\n");
            ld(f, A_INPR, "r8");
            zero(f, A_FGI);
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF400:
            mano_emit(f, "    and r0, r8, #255\n"
                         "    str r0, [r5, #36]\n"
                         "    mov r0, #1\n"
                         "    add r1, r5, #36\n"
                         "    mov r2, #1\n"
                         "    mov r7, #4\n"
                         "    svc #0\n");
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF200:
            ld(f, A_FGI, "r0");
            mano_emit(f, "    cmp r0, #0\n    bne 2f\n");
            zero(f, A_INPR);
            mano_emit(f, "    mov r0, #0\n"
                         "    add r1, r5, #32\n"
                         "    mov r2, #1\n"
                         "    mov r7, #3\n"
                         "    svc #0\n");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               mano_emit(f, "    cmp r0, #0\n");
               a32_branch(f, prog, "ble", bun.addr);
               mano_emit(f, "    mov r0, #1\n");
               st(f, A_FGI, "r0");
               mano_emit(f, "2:\n");
               a32_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               mano_emit(f, "    cmp r0, #0\n    ble 3f\n"
                            "    mov r0, #1\n");
               st(f, A_FGI, "r0");
               mano_emit(f, "2:\n");
               a32_branch(f, prog, "b", addr + 2);
               mano_emit(f, "3:\n");
               a32_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0xF100:
            a32_fallthrough(f, prog, addr + 2, next_emitted);
            break;
         case 0xF080:
         case 0xF040:
            a32_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         default:
            break;
         }
      }
   }

   emit_bad_epilogue(f, badlen);
}

void mano_emit_arm32(FILE* f, const ManoProgram* prog, int start_pc,
                     int opt_level) {
   if (opt_level <= 0)
      emit_arm32_o0(f, prog, start_pc);
   else if (opt_level == 1)
      emit_arm32_o1(f, prog, start_pc);
   else
      emit_arm32_opt(f, prog, start_pc, opt_level);
}
