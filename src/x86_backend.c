#define _POSIX_C_SOURCE 200809L

#include "backend_int.h"
#include "isa.h"

#include <string.h>

/* =========================================================================
 * Low-level helpers shared by all optimization levels
 * ========================================================================= */

static void ld(FILE* f, bool x64, const char* reg, const char* sym) {
   if (x64)
      mano_emit(f, "    mov %s, dword ptr [rip+%s]\n", reg, sym);
   else
      mano_emit(f, "    mov %s, dword ptr [%s]\n", reg, sym);
}

static void st(FILE* f, bool x64, const char* sym, const char* reg) {
   if (x64)
      mano_emit(f, "    mov dword ptr [rip+%s], %s\n", sym, reg);
   else
      mano_emit(f, "    mov dword ptr [%s], %s\n", sym, reg);
}

static void ldi(FILE* f, bool x64, const char* sym, int val) {
   if (x64)
      mano_emit(f, "    mov dword ptr [rip+%s], %d\n", sym, val);
   else
      mano_emit(f, "    mov dword ptr [%s], %d\n", sym, val);
}

static void sys_read1(FILE* f, bool x64, const char* sym) {
   if (x64)
      mano_emit(f, "    xor eax, eax\n"
                   "    xor edi, edi\n"
                   "    lea rsi, [rip+%s]\n"
                   "    mov edx, 1\n"
                   "    syscall\n",
                sym);
   else
      mano_emit(f, "    push ebx\n"
                   "    mov eax, 3\n"
                   "    xor ebx, ebx\n"
                   "    lea ecx, [%s]\n"
                   "    mov edx, 1\n"
                   "    int 0x80\n"
                   "    pop ebx\n",
                sym);
}

static void sys_write1(FILE* f, bool x64, const char* sym) {
   if (x64)
      mano_emit(f, "    mov eax, 1\n"
                   "    mov edi, 1\n"
                   "    lea rsi, [rip+%s]\n"
                   "    mov edx, 1\n"
                   "    syscall\n",
                sym);
   else
      mano_emit(f, "    push ebx\n"
                   "    mov eax, 4\n"
                   "    mov ebx, 1\n"
                   "    lea ecx, [%s]\n"
                   "    mov edx, 1\n"
                   "    int 0x80\n"
                   "    pop ebx\n",
                sym);
}

static void sys_exit(FILE* f, bool x64, int code) {
   if (x64) {
      if (code)
         mano_emit(f, "    mov edi, %d\n", code);
      else
         mano_emit(f, "    xor edi, edi\n");
      mano_emit(f, "    mov eax, 60\n    syscall\n");
   }
   else {
      if (code)
         mano_emit(f, "    mov ebx, %d\n", code);
      else
         mano_emit(f, "    xor ebx, ebx\n");
      mano_emit(f, "    mov eax, 1\n    int 0x80\n");
   }
}

static void emit_bad_epilogue(FILE* f, bool x64, int badlen) {
   mano_emit(f, "\n.Lbad:\n");
   if (x64)
      mano_emit(f,
                "    mov eax, 1\n"
                "    mov edi, 2\n"
                "    lea rsi, [rip+.Lbadmsg]\n"
                "    mov edx, %d\n"
                "    syscall\n",
                badlen);
   else
      mano_emit(f,
                "    mov eax, 4\n"
                "    mov ebx, 2\n"
                "    lea ecx, [.Lbadmsg]\n"
                "    mov edx, %d\n"
                "    int 0x80\n",
                badlen);
   sys_exit(f, x64, 1);
   mano_emit(f, "\n.section .note.GNU-stack,\"\",@progbits\n");
}

static void emit_src_comment(FILE* f, const ManoProgram* prog, int addr) {
   int32_t li = prog->line_of[addr];
   if (li >= 0 && (size_t)li < prog->nlines)
      mano_emit(f, "\n# %03X: %s\n", addr, prog->lines[li].text);
   else
      mano_emit(f, "\n# %03X:\n", addr);
}

/* =========================================================================
 * -O0: Literal micro-ops (full Mano register state in .bss + .Ldispatch)
 * ========================================================================= */

static void o0_fetch(FILE* f, bool x64, int addr, bool indirect) {
   const char* ib = x64 ? "rbx" : "esi";
   const char* ix = x64 ? "rax" : "eax";

   mano_emit(f, "\n.Lx%03X:\n", addr);
   ld(f, x64, "eax", ".Lpc");
   mano_emit(f, "    movzx ecx, word ptr [%s+%s*2]\n", ib, ix);
   st(f, x64, ".Lir", "ecx");
   mano_emit(f, "    inc eax\n    and eax, 4095\n");
   st(f, x64, ".Lpc", "eax");
   mano_emit(f, "    mov eax, ecx\n    and eax, 4095\n");
   st(f, x64, ".Lar", "eax");
   if (indirect) {
      mano_emit(f, "    movzx eax, word ptr [%s+%s*2]\n"
                   "    and eax, 4095\n",
                ib, ix);
      st(f, x64, ".Lar", "eax");
   }
}

static void o0_incpc(FILE* f, bool x64) {
   ld(f, x64, "edx", ".Lpc");
   mano_emit(f, "    inc edx\n    and edx, 4095\n");
   st(f, x64, ".Lpc", "edx");
}

static void o0_mem_op(FILE* f, bool x64, const ManoInstr* ins) {
   const char* ib = x64 ? "rbx" : "esi";
   const char* ix = x64 ? "rax" : "eax";

   switch (ins->op) {
   case 0:
      ld(f, x64, "eax", ".Lar");
      mano_emit(f, "    movzx eax, word ptr [%s+%s*2]\n", ib, ix);
      st(f, x64, ".Ldr", "eax");
      ld(f, x64, "ecx", ".Lac");
      mano_emit(f, "    and ecx, eax\n");
      st(f, x64, ".Lac", "ecx");
      break;
   case 1:
      ld(f, x64, "eax", ".Lar");
      mano_emit(f, "    movzx eax, word ptr [%s+%s*2]\n", ib, ix);
      st(f, x64, ".Ldr", "eax");
      ld(f, x64, "ecx", ".Lac");
      mano_emit(f,
                "    add ecx, eax\n"
                "    xor edx, edx\n"
                "    cmp ecx, 65535\n"
                "    seta dl\n");
      st(f, x64, ".Le", "edx");
      mano_emit(f, "    and ecx, 65535\n");
      st(f, x64, ".Lac", "ecx");
      break;
   case 2:
      ld(f, x64, "eax", ".Lar");
      mano_emit(f, "    movzx eax, word ptr [%s+%s*2]\n", ib, ix);
      st(f, x64, ".Ldr", "eax");
      st(f, x64, ".Lac", "eax");
      break;
   case 3:
      ld(f, x64, "eax", ".Lar");
      ld(f, x64, "ecx", ".Lac");
      mano_emit(f, "    mov word ptr [%s+%s*2], cx\n", ib, ix);
      break;
   case 4:
      ld(f, x64, "eax", ".Lar");
      st(f, x64, ".Lpc", "eax");
      break;
   case 5:
      ld(f, x64, "eax", ".Lar");
      ld(f, x64, "ecx", ".Lpc");
      mano_emit(f, "    mov word ptr [%s+%s*2], cx\n"
                   "    lea ecx, [%s+1]\n"
                   "    and ecx, 4095\n",
                ib, ix, ix);
      st(f, x64, ".Lpc", "ecx");
      break;
   case 6:
      ld(f, x64, "eax", ".Lar");
      mano_emit(f, "    movzx ecx, word ptr [%s+%s*2]\n"
                   "    inc ecx\n"
                   "    mov word ptr [%s+%s*2], cx\n"
                   "    test cx, cx\n"
                   "    jne 1f\n",
                ib, ix, ib, ix);
      o0_incpc(f, x64);
      mano_emit(f, "1:\n");
      break;
   default:
      break;
   }
}

static void o0_reg_op(FILE* f, bool x64, int code) {
   switch (code) {
   case 0x7800:
      ldi(f, x64, ".Lac", 0);
      break;
   case 0x7400:
      ldi(f, x64, ".Le", 0);
      break;
   case 0x7200:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    not eax\n    and eax, 65535\n");
      st(f, x64, ".Lac", "eax");
      break;
   case 0x7100:
      ld(f, x64, "eax", ".Le");
      mano_emit(f, "    xor eax, 1\n");
      st(f, x64, ".Le", "eax");
      break;
   case 0x7080:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    mov ecx, eax\n    and ecx, 1\n    shr eax, 1\n");
      ld(f, x64, "edx", ".Le");
      mano_emit(f, "    shl edx, 15\n    or eax, edx\n");
      st(f, x64, ".Lac", "eax");
      st(f, x64, ".Le", "ecx");
      break;
   case 0x7040:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    mov ecx, eax\n    shr ecx, 15\n"
                   "    shl eax, 1\n    and eax, 65535\n");
      ld(f, x64, "edx", ".Le");
      mano_emit(f, "    or eax, edx\n");
      st(f, x64, ".Lac", "eax");
      st(f, x64, ".Le", "ecx");
      break;
   case 0x7020:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    inc eax\n    and eax, 65535\n");
      st(f, x64, ".Lac", "eax");
      break;
   case 0x7010:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    test eax, 32768\n    jne 1f\n");
      o0_incpc(f, x64);
      mano_emit(f, "1:\n");
      break;
   case 0x7008:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    test eax, 32768\n    je 1f\n");
      o0_incpc(f, x64);
      mano_emit(f, "1:\n");
      break;
   case 0x7004:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    test eax, eax\n    jne 1f\n");
      o0_incpc(f, x64);
      mano_emit(f, "1:\n");
      break;
   case 0x7002:
      ld(f, x64, "eax", ".Le");
      mano_emit(f, "    test eax, eax\n    jne 1f\n");
      o0_incpc(f, x64);
      mano_emit(f, "1:\n");
      break;
   case 0x7001:
      sys_exit(f, x64, 0);
      break;
   default:
      break;
   }
}

static void o0_io_op(FILE* f, bool x64, int code) {
   const char* res = x64 ? "rax" : "eax";

   switch (code) {
   case 0xF800:
      ld(f, x64, "eax", ".Lfgi");
      mano_emit(f, "    test eax, eax\n    jne 2f\n");
      ldi(f, x64, ".Linpr", 0);
      sys_read1(f, x64, ".Linpr");
      ldi(f, x64, ".Lfgi", 1);
      mano_emit(f, "2:\n");
      ld(f, x64, "eax", ".Linpr");
      st(f, x64, ".Lac", "eax");
      ldi(f, x64, ".Lfgi", 0);
      break;
   case 0xF400:
      ld(f, x64, "eax", ".Lac");
      mano_emit(f, "    and eax, 255\n");
      st(f, x64, ".Loutr", "eax");
      sys_write1(f, x64, ".Loutr");
      break;
   case 0xF200:
      ld(f, x64, "eax", ".Lfgi");
      mano_emit(f, "    test eax, eax\n    jne 2f\n");
      ldi(f, x64, ".Linpr", 0);
      sys_read1(f, x64, ".Linpr");
      mano_emit(f, "    cmp %s, 0\n    jle 2f\n", res);
      ldi(f, x64, ".Lfgi", 1);
      mano_emit(f, "2:\n");
      ld(f, x64, "eax", ".Lfgi");
      mano_emit(f, "    test eax, eax\n    je 3f\n");
      o0_incpc(f, x64);
      mano_emit(f, "3:\n");
      break;
   case 0xF100:
      o0_incpc(f, x64);
      break;
   case 0xF080:
      ldi(f, x64, ".Lien", 1);
      break;
   case 0xF040:
      ldi(f, x64, ".Lien", 0);
      break;
   default:
      break;
   }
}

static void emit_x86_o0(FILE* f, const ManoProgram* prog, int start_pc,
                        bool x64) {
   mano_emit(f, ".intel_syntax noprefix\n");
   mano_emit_dataseg(f, prog);

   mano_emit(f, "\n.section .bss\n.balign 16\n"
                ".Lac:   .space 4\n"
                ".Lpc:   .space 4\n"
                ".Lir:   .space 4\n"
                ".Lar:   .space 4\n"
                ".Ldr:   .space 4\n"
                ".Le:    .space 4\n"
                ".Lien:  .space 4\n"
                ".Lfgi:  .space 4\n"
                ".Linpr: .space 4\n"
                ".Loutr: .space 4\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, x64 ? "quad" : "long");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, @function\n"
                "_start:\n");
   if (x64) {
      mano_emit(f, "    lea rbx, [rip+.Lmem]\n"
                   "    mov dword ptr [rip+.Lpc], %d\n",
                start_pc & 0x0FFF);
   }
   else {
      mano_emit(f, "    lea esi, [.Lmem]\n"
                   "    mov dword ptr [.Lpc], %d\n",
                start_pc & 0x0FFF);
   }
   mano_emit(f, "    jmp .Ldispatch\n\n.Ldispatch:\n");
   if (x64)
      mano_emit(f,
                "    mov eax, dword ptr [rip+.Lpc]\n"
                "    and eax, 4095\n"
                "    lea rcx, [rip+.Ljtab]\n"
                "    jmp qword ptr [rcx+rax*8]\n");
   else
      mano_emit(f,
                "    mov eax, dword ptr [.Lpc]\n"
                "    and eax, 4095\n"
                "    lea ecx, [.Ljtab]\n"
                "    jmp dword ptr [ecx+eax*4]\n");

   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      if (!prog->is_code[addr])
         continue;

      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;

      emit_src_comment(f, prog, addr);
      o0_fetch(f, x64, addr, ins.indirect != 0);

      if (ins.kind == MANO_KIND_MEM) {
         o0_mem_op(f, x64, &ins);
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         o0_reg_op(f, x64, code);
         if (code == 0x7001)
            continue;
      }
      else {
         o0_io_op(f, x64, mano_mnemonics[ins.op].code);
      }
      mano_emit(f, "    jmp .Ldispatch\n");
   }

   emit_bad_epilogue(f, x64, badlen);
}

/* =========================================================================
 * -O1: Register-cached AC/E (+ PC/Ljtab on amd64), dynamic fetch & dispatch
 * ========================================================================= */

static void o1_incpc(FILE* f, bool x64) {
   if (x64) {
      mano_emit(f, "    inc r14d\n    and r14d, 4095\n");
   }
   else {
      mano_emit(f, "    mov eax, dword ptr [.Lpc]\n"
                   "    inc eax\n    and eax, 4095\n"
                   "    mov dword ptr [.Lpc], eax\n");
   }
}

static void o1_set_e_from_cf(FILE* f, bool x64) {
   if (x64)
      mano_emit(f, "    setc r13b\n");
   else
      mano_emit(f, "    setc dl\n    movzx ebp, dl\n");
}

static void emit_x86_o1(FILE* f, const ManoProgram* prog, int start_pc,
                        bool x64) {
   const char* ib = x64 ? "rbx" : "esi";
   const char* ac32 = x64 ? "r12d" : "edi";
   const char* ac16 = x64 ? "r12w" : "di";
   const char* e32 = x64 ? "r13d" : "ebp";
   const char* arx = x64 ? "rcx" : "ecx";

   mano_emit(f, ".intel_syntax noprefix\n");
   mano_emit_dataseg(f, prog);

   mano_emit(f, "\n.section .bss\n.balign 16\n");
   if (!x64)
      mano_emit(f, ".Lpc:   .space 4\n");
   mano_emit(f, ".Lfgi:  .space 4\n"
                ".Linpr: .space 4\n"
                ".Loutr: .space 4\n");

   int badlen = mano_emit_badmsg(f);
   mano_emit_jumptable(f, prog, x64 ? "quad" : "long");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, @function\n"
                "_start:\n");
   if (x64) {
      mano_emit(f, "    lea rbx, [rip+.Lmem]\n"
                   "    lea r15, [rip+.Ljtab]\n"
                   "    xor r12d, r12d\n"
                   "    xor r13d, r13d\n"
                   "    mov r14d, %d\n"
                   "\n.Ldispatch:\n"
                   "    jmp qword ptr [r15+r14*8]\n",
                start_pc & 0x0FFF);
   }
   else {
      mano_emit(f, "    lea esi, [.Lmem]\n"
                   "    xor edi, edi\n"
                   "    xor ebp, ebp\n"
                   "    mov dword ptr [.Lpc], %d\n"
                   "\n.Ldispatch:\n"
                   "    mov eax, dword ptr [.Lpc]\n"
                   "    lea ecx, [.Ljtab]\n"
                   "    jmp dword ptr [ecx+eax*4]\n",
                start_pc & 0x0FFF);
   }

   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      if (!prog->is_code[addr])
         continue;

      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;

      emit_src_comment(f, prog, addr);
      mano_emit(f, ".Lx%03X:\n", addr);

      if (ins.kind == MANO_KIND_MEM) {
         if (x64) {
            mano_emit(f, "    movzx ecx, word ptr [rbx+r14*2]\n"
                         "    inc r14d\n    and r14d, 4095\n"
                         "    and ecx, 4095\n");
         }
         else {
            mano_emit(f, "    mov eax, dword ptr [.Lpc]\n"
                         "    movzx ecx, word ptr [esi+eax*2]\n"
                         "    inc eax\n    and eax, 4095\n"
                         "    mov dword ptr [.Lpc], eax\n"
                         "    and ecx, 4095\n");
         }
         if (ins.indirect) {
            mano_emit(f, "    movzx ecx, word ptr [%s+%s*2]\n"
                         "    and ecx, 4095\n",
                      ib, arx);
         }

         switch (ins.op) {
         case 0: /* AND */
            mano_emit(f, "    and %s, word ptr [%s+%s*2]\n", ac16, ib, arx);
            break;
         case 1: /* ADD */
            mano_emit(f, "    add %s, word ptr [%s+%s*2]\n", ac16, ib, arx);
            o1_set_e_from_cf(f, x64);
            break;
         case 2: /* LDA */
            mano_emit(f, "    movzx %s, word ptr [%s+%s*2]\n",
                      ac32, ib, arx);
            break;
         case 3: /* STA */
            mano_emit(f, "    mov word ptr [%s+%s*2], %s\n", ib, arx, ac16);
            break;
         case 4: /* BUN */
            if (x64)
               mano_emit(f, "    mov r14d, ecx\n");
            else
               mano_emit(f, "    mov dword ptr [.Lpc], ecx\n");
            break;
         case 5: /* BSA */
            if (x64) {
               mano_emit(f, "    mov word ptr [rbx+rcx*2], r14w\n"
                            "    lea r14d, [rcx+1]\n"
                            "    and r14d, 4095\n");
            }
            else {
               mano_emit(f, "    mov eax, dword ptr [.Lpc]\n"
                            "    mov word ptr [esi+ecx*2], ax\n"
                            "    lea eax, [ecx+1]\n"
                            "    and eax, 4095\n"
                            "    mov dword ptr [.Lpc], eax\n");
            }
            break;
         case 6: /* ISZ */
            mano_emit(f, "    inc word ptr [%s+%s*2]\n"
                         "    jnz 1f\n",
                      ib, arx);
            o1_incpc(f, x64);
            mano_emit(f, "1:\n");
            break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0x7001) {
            sys_exit(f, x64, 0);
            continue;
         }
         o1_incpc(f, x64);
         switch (code) {
         case 0x7800:
            mano_emit(f, "    xor %s, %s\n", ac32, ac32);
            break;
         case 0x7400:
            mano_emit(f, "    xor %s, %s\n", e32, e32);
            break;
         case 0x7200:
            mano_emit(f, "    not %s\n", ac16);
            break;
         case 0x7100:
            mano_emit(f, "    xor %s, 1\n", e32);
            break;
         case 0x7080:
            mano_emit(f, "    bt %s, 0\n    rcr %s, 1\n", e32, ac16);
            o1_set_e_from_cf(f, x64);
            break;
         case 0x7040:
            mano_emit(f, "    bt %s, 0\n    rcl %s, 1\n", e32, ac16);
            o1_set_e_from_cf(f, x64);
            break;
         case 0x7020:
            mano_emit(f, "    inc %s\n", ac16);
            break;
         case 0x7010:
            mano_emit(f, "    test %s, %s\n    js 1f\n", ac16, ac16);
            o1_incpc(f, x64);
            mano_emit(f, "1:\n");
            break;
         case 0x7008:
            mano_emit(f, "    test %s, %s\n    jns 1f\n", ac16, ac16);
            o1_incpc(f, x64);
            mano_emit(f, "1:\n");
            break;
         case 0x7004:
            mano_emit(f, "    test %s, %s\n    jnz 1f\n", ac16, ac16);
            o1_incpc(f, x64);
            mano_emit(f, "1:\n");
            break;
         case 0x7002:
            mano_emit(f, "    test %s, %s\n    jnz 1f\n", e32, e32);
            o1_incpc(f, x64);
            mano_emit(f, "1:\n");
            break;
         default:
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         const char* res = x64 ? "rax" : "eax";
         o1_incpc(f, x64);
         switch (code) {
         case 0xF800:
            ld(f, x64, "eax", ".Lfgi");
            mano_emit(f, "    test eax, eax\n    jne 2f\n");
            ldi(f, x64, ".Linpr", 0);
            sys_read1(f, x64, ".Linpr");
            mano_emit(f, "2:\n");
            ld(f, x64, ac32, ".Linpr");
            ldi(f, x64, ".Lfgi", 0);
            break;
         case 0xF400:
            if (x64) {
               mano_emit(f, "    mov byte ptr [rip+.Loutr], r12b\n");
            }
            else {
               mano_emit(f, "    mov eax, edi\n    and eax, 255\n"
                            "    mov dword ptr [.Loutr], eax\n");
            }
            sys_write1(f, x64, ".Loutr");
            break;
         case 0xF200:
            ld(f, x64, "eax", ".Lfgi");
            mano_emit(f, "    test eax, eax\n    jne 2f\n");
            ldi(f, x64, ".Linpr", 0);
            sys_read1(f, x64, ".Linpr");
            mano_emit(f, "    cmp %s, 0\n    jle 3f\n", res);
            ldi(f, x64, ".Lfgi", 1);
            mano_emit(f, "2:\n");
            o1_incpc(f, x64);
            mano_emit(f, "3:\n");
            break;
         case 0xF100:
            o1_incpc(f, x64);
            break;
         case 0xF080:
         case 0xF040:
            break;
         default:
            break;
         }
      }

      mano_emit(f, "    jmp .Ldispatch\n");
   }

   emit_bad_epilogue(f, x64, badlen);
}

/* =========================================================================
 * -O2 & -O3: Direct native control flow, static operands, register promotion,
 *            constant folding, dead E elimination, and peephole fusion
 * ========================================================================= */

static const char* const x64_prom32[MANO_MAX_PROMOTED] = {
    "r14d", "r15d", "ebp", "r8d", "r9d", "r10d"};
static const char* const x64_prom16[MANO_MAX_PROMOTED] = {
    "r14w", "r15w", "bp", "r8w", "r9w", "r10w"};

static const char* const x32_prom32[2] = {"ebx", "ebp"};
static const char* const x32_prom16[2] = {"bx", "bp"};

static const char* prom32(bool x64, int slot) {
   return x64 ? x64_prom32[slot] : x32_prom32[slot];
}

static const char* prom16(bool x64, int slot) {
   return x64 ? x64_prom16[slot] : x32_prom16[slot];
}

static void emit_target_jmp(FILE* f, const ManoProgram* prog,
                            const char* jcc, int target) {
   target &= MANO_ADDR_MASK;
   if (prog->is_code[target])
      mano_emit(f, "    %s .Lx%03X\n", jcc, target);
   else
      mano_emit(f, "    %s .Lbad\n", jcc);
}

static void emit_fallthrough(FILE* f, const ManoProgram* prog, int next,
                             int next_emitted) {
   next &= MANO_ADDR_MASK;
   if (next == next_emitted)
      return;
   emit_target_jmp(f, prog, "jmp", next);
}

static bool can_fuse_next(const ManoProgram* prog, const ManoAnalysis* an,
                          int opt_level, int addr, ManoInstr* next_ins) {
   if (opt_level < 3)
      return false;
   int nxt = (addr + 1) & MANO_ADDR_MASK;
   if (nxt != addr + 1 || !an->reachable[nxt] || an->is_target[nxt])
      return false;
   return mano_decode(prog->mem[nxt], next_ins);
}

static bool is_direct_bun(const ManoInstr* ins, int* target) {
   if (ins->kind == MANO_KIND_MEM && ins->op == 4 && !ins->indirect) {
      *target = ins->addr & MANO_ADDR_MASK;
      return true;
   }
   return false;
}

static int find_next_emitted(const ManoAnalysis* an, const uint8_t* skip_emit,
                             int after) {
   for (int a = after + 1; a < MANO_MEM_WORDS; a++) {
      if (an->reachable[a] && !skip_emit[a])
         return a;
   }
   return -1;
}

static void emit_x86_opt(FILE* f, const ManoProgram* prog, int start_pc,
                         bool x64, int opt_level) {
   ManoAnalysis an;
   mano_analyze(prog, start_pc, opt_level, x64 ? MANO_MAX_PROMOTED : 2, &an);

   /* On i386, ebp holds E when need_e is true, so only slot 0 (ebx) is free. */
   if (!x64 && an.need_e && an.num_promoted > 1) {
      int dropped = an.promoted_addr[1];
      an.slot_reg[dropped] = -1;
      an.is_counter[dropped] = 0;
      an.num_promoted = 1;
      an.need_mem = true;
   }

   const char* ib = x64 ? "rbx" : "esi";
   const char* ac32 = x64 ? "r12d" : "edi";
   const char* ac16 = x64 ? "r12w" : "di";
   const char* e32 = x64 ? "r13d" : "ebp";
   const char* ix = x64 ? "rax" : "eax";

   /* Pre-pass: determine which instructions will be fused away at -O3 so
    * fallthrough detection knows the exact next emitted instruction. */
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
            if (can_fuse_next(prog, &an, opt_level, addr, &n1) &&
                is_direct_bun(&n1, &bun_t)) {
               fused_away[addr + 1] = 1;
            }
         }
         else if (ins.kind == MANO_KIND_MEM && ins.op == 2 &&
                  !ins.indirect) {
            ManoInstr n2;
            int a = ins.addr & MANO_ADDR_MASK;
            if (!an.need_e && an.slot_reg[a] >= 0 &&
                can_fuse_next(prog, &an, opt_level, addr, &n1) &&
                n1.kind == MANO_KIND_MEM && n1.op == 1 && !n1.indirect &&
                can_fuse_next(prog, &an, opt_level, addr + 1, &n2) &&
                n2.kind == MANO_KIND_MEM && n2.op == 3 && !n2.indirect &&
                ins.addr == n2.addr) {
               fused_away[addr + 1] = 1;
               fused_away[addr + 2] = 1;
            }
            else if (an.is_const[a] &&
                     can_fuse_next(prog, &an, opt_level, addr, &n1) &&
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
               if (can_fuse_next(prog, &an, opt_level, addr, &n1) &&
                   is_direct_bun(&n1, &bun_t)) {
                  fused_away[addr + 1] = 1;
               }
            }
            else if (code == 0x7800 || code == 0x7020) {
               int cur = addr;
               while (can_fuse_next(prog, &an, opt_level, cur, &n1) &&
                      n1.kind == MANO_KIND_REG &&
                      mano_mnemonics[n1.op].code == 0x7020) {
                  fused_away[cur + 1] = 1;
                  cur++;
               }
            }
            else if (code == 0x7200) {
               if (can_fuse_next(prog, &an, opt_level, addr, &n1) &&
                   n1.kind == MANO_KIND_REG &&
                   mano_mnemonics[n1.op].code == 0x7020) {
                  fused_away[addr + 1] = 1;
               }
            }
         }
         else if (ins.kind == MANO_KIND_IO) {
            int code = mano_mnemonics[ins.op].code;
            if (code == 0xF200) {
               int bun_t = 0;
               if (can_fuse_next(prog, &an, opt_level, addr, &n1) &&
                   is_direct_bun(&n1, &bun_t)) {
                  fused_away[addr + 1] = 1;
               }
            }
         }
      }
   }

   mano_emit(f, ".intel_syntax noprefix\n");
   if (an.need_mem)
      mano_emit_dataseg(f, prog);

   if (an.need_io_state) {
      mano_emit(f, "\n.section .bss\n.balign 16\n"
                   ".Lfgi:  .space 4\n"
                   ".Linpr: .space 4\n"
                   ".Loutr: .space 4\n");
   }

   int badlen = mano_emit_badmsg(f);
   if (an.need_jtab)
      mano_emit_jumptable(f, prog, x64 ? "quad" : "long");

   mano_emit(f, "\n.section .text\n"
                ".globl _start\n"
                ".type _start, @function\n"
                "_start:\n");
   if (an.need_mem) {
      if (x64)
         mano_emit(f, "    lea rbx, [rip+.Lmem]\n");
      else
         mano_emit(f, "    lea esi, [.Lmem]\n");
   }
   mano_emit(f, "    xor %s, %s\n", ac32, ac32);
   if (an.need_e)
      mano_emit(f, "    xor %s, %s\n", e32, e32);
   for (int i = 0; i < an.num_promoted; i++) {
      int paddr = an.promoted_addr[i];
      uint32_t initv = an.is_counter[paddr]
                           ? (65536u - (uint32_t)prog->mem[paddr])
                           : (uint32_t)prog->mem[paddr];
      const char* r32 = prom32(x64, i);
      if (initv == 0)
         mano_emit(f, "    xor %s, %s\n", r32, r32);
      else
         mano_emit(f, "    mov %s, %u\n", r32, (unsigned)initv);
   }

   int first_emitted = find_next_emitted(&an, fused_away, -1);
   int entry = start_pc & MANO_ADDR_MASK;
   if (entry != first_emitted || !prog->is_code[entry])
      emit_target_jmp(f, prog, "jmp", entry);

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

      int next_emitted = find_next_emitted(&an, fused_away, addr);

      if (ins.kind == MANO_KIND_MEM) {
         int a = ins.addr & MANO_ADDR_MASK;
         int slot = (!ins.indirect) ? an.slot_reg[a] : -1;
         bool is_c = (!ins.indirect && an.is_const[a]);
         uint16_t cval = prog->mem[a];

         /* Fused LDA X; ADD Y; STA X (32-bit modular add, zero partial-reg stall) */
         if (ins.op == 2 && !ins.indirect && slot >= 0 && !an.need_e &&
             addr + 2 < MANO_MEM_WORDS &&
             fused_away[addr + 1] && fused_away[addr + 2]) {
            ManoInstr add_ins;
            mano_decode(prog->mem[addr + 1], &add_ins);
            int ya = add_ins.addr & MANO_ADDR_MASK;
            const char* rx32 = prom32(x64, slot);
            if (an.is_const[ya]) {
               uint16_t yv = prog->mem[ya];
               if (yv != 0)
                  mano_emit(f, "    add %s, %u\n", rx32, (unsigned)yv);
            }
            else if (an.slot_reg[ya] >= 0) {
               mano_emit(f, "    add %s, %s\n",
                         rx32, prom32(x64, an.slot_reg[ya]));
            }
            else {
               mano_emit(f, "    movzx eax, word ptr [%s+%d]\n"
                            "    add %s, eax\n",
                         ib, ya * 2, rx32);
            }
            if (an.ac_live_out[addr + 2])
               mano_emit(f, "    movzx %s, %s\n", ac32, prom16(x64, slot));
            emit_fallthrough(f, prog, addr + 3, next_emitted);
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
               if (an.ac_live_out[addr + 1]) {
                  if (res == 0)
                     mano_emit(f, "    xor %s, %s\n", ac32, ac32);
                  else
                     mano_emit(f, "    mov %s, %u\n",
                               ac32, (unsigned)res);
               }
            }
            else if (n1.op == 3) {
               if (!an.is_dead_store[a1]) {
                  int ys = an.slot_reg[a1];
                  uint32_t sv = an.is_counter[a1]
                                    ? (65536u - (uint32_t)cval)
                                    : (uint32_t)cval;
                  const char* ry32 = prom32(x64, ys);
                  if (sv == 0)
                     mano_emit(f, "    xor %s, %s\n", ry32, ry32);
                  else
                     mano_emit(f, "    mov %s, %u\n",
                               ry32, (unsigned)sv);
               }
               if (an.ac_live_out[addr + 1]) {
                  if (cval == 0)
                     mano_emit(f, "    xor %s, %s\n", ac32, ac32);
                  else
                     mano_emit(f, "    mov %s, %u\n",
                               ac32, (unsigned)cval);
               }
            }
            emit_fallthrough(f, prog, addr + 2, next_emitted);
            continue;
         }

         if (ins.indirect) {
            mano_emit(f, "    movzx eax, word ptr [%s+%d]\n"
                         "    and eax, 4095\n",
                      ib, a * 2);
            switch (ins.op) {
            case 0:
               mano_emit(f, "    and %s, word ptr [%s+%s*2]\n",
                         ac16, ib, ix);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 1:
               mano_emit(f, "    add %s, word ptr [%s+%s*2]\n",
                         ac16, ib, ix);
               if (an.need_e)
                  o1_set_e_from_cf(f, x64);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 2:
               mano_emit(f, "    movzx %s, word ptr [%s+%s*2]\n",
                         ac32, ib, ix);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 3:
               mano_emit(f, "    mov word ptr [%s+%s*2], %s\n",
                         ib, ix, ac16);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            case 4: /* BUN I */
               if (x64)
                  mano_emit(f, "    lea rcx, [rip+.Ljtab]\n"
                               "    jmp qword ptr [rcx+rax*8]\n");
               else
                  mano_emit(f, "    lea ecx, [.Ljtab]\n"
                               "    jmp dword ptr [ecx+eax*4]\n");
               break;
            case 5: /* BSA I */
               mano_emit(f, "    mov word ptr [%s+%s*2], %d\n"
                            "    inc eax\n    and eax, 4095\n",
                         ib, ix, (addr + 1) & MANO_ADDR_MASK);
               if (x64)
                  mano_emit(f, "    lea rcx, [rip+.Ljtab]\n"
                               "    jmp qword ptr [rcx+rax*8]\n");
               else
                  mano_emit(f, "    lea ecx, [.Ljtab]\n"
                               "    jmp dword ptr [ecx+eax*4]\n");
               break;
            case 6: /* ISZ I */
               mano_emit(f, "    inc word ptr [%s+%s*2]\n", ib, ix);
               emit_target_jmp(f, prog, "jz", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
               break;
            default:
               break;
            }
            continue;
         }

         /* Direct memory instruction */
         switch (ins.op) {
         case 0: /* AND */
            if (is_c)
               mano_emit(f, "    and %s, %u\n", ac16, (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    and %s, %s\n", ac16, prom16(x64, slot));
            else
               mano_emit(f, "    and %s, word ptr [%s+%d]\n",
                         ac16, ib, a * 2);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 1: /* ADD */
            if (is_c)
               mano_emit(f, "    add %s, %u\n", ac16, (unsigned)cval);
            else if (slot >= 0)
               mano_emit(f, "    add %s, %s\n", ac16, prom16(x64, slot));
            else
               mano_emit(f, "    add %s, word ptr [%s+%d]\n",
                         ac16, ib, a * 2);
            if (an.need_e)
               o1_set_e_from_cf(f, x64);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 2: /* LDA */
            if (opt_level < 3 || an.ac_live_out[addr]) {
               if (is_c) {
                  if (cval == 0)
                     mano_emit(f, "    xor %s, %s\n", ac32, ac32);
                  else
                     mano_emit(f, "    mov %s, %u\n",
                               ac32, (unsigned)cval);
               }
               else if (slot >= 0) {
                  mano_emit(f, "    movzx %s, %s\n",
                            ac32, prom16(x64, slot));
               }
               else {
                  mano_emit(f, "    movzx %s, word ptr [%s+%d]\n",
                            ac32, ib, a * 2);
               }
            }
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 3: /* STA */
            if (an.is_dead_store[a]) {
               /* Write-only slot: eliminated at -O3. */
            }
            else if (an.is_counter[a]) {
               const char* r32 = prom32(x64, slot);
               mano_emit(f, "    mov %s, 65536\n    sub %s, %s\n",
                         r32, r32, ac32);
            }
            else if (slot >= 0) {
               mano_emit(f, "    mov %s, %s\n", prom32(x64, slot), ac32);
            }
            else {
               mano_emit(f, "    mov word ptr [%s+%d], %s\n",
                         ib, a * 2, ac16);
            }
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 4: /* BUN */
            emit_target_jmp(f, prog, "jmp", a);
            break;
         case 5: /* BSA */
            mano_emit(f, "    mov word ptr [%s+%d], %d\n",
                      ib, a * 2, (addr + 1) & MANO_ADDR_MASK);
            emit_target_jmp(f, prog, "jmp", a + 1);
            break;
         case 6: /* ISZ */
            if (an.is_counter[a]) {
               const char* r32 = prom32(x64, slot);
               mano_emit(f, "    sub %s, 1\n", r32);
               if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
                  ManoInstr bun;
                  mano_decode(prog->mem[addr + 1], &bun);
                  emit_target_jmp(f, prog, "jnz", bun.addr);
                  mano_emit(f, "    mov %s, 65536\n", r32);
                  emit_fallthrough(f, prog, addr + 2, next_emitted);
               }
               else {
                  mano_emit(f, "    jnz 1f\n    mov %s, 65536\n", r32);
                  emit_target_jmp(f, prog, "jmp", addr + 2);
                  mano_emit(f, "1:\n");
                  emit_fallthrough(f, prog, addr + 1, next_emitted);
               }
               break;
            }
            if (slot >= 0)
               mano_emit(f, "    inc %s\n", prom16(x64, slot));
            else
               mano_emit(f, "    inc word ptr [%s+%d]\n", ib, a * 2);
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               emit_target_jmp(f, prog, "jnz", bun.addr);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               emit_target_jmp(f, prog, "jz", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         switch (code) {
         case 0x7800: { /* CLA (+ fused INC*) */
            int count = 0;
            while (addr + count + 1 < MANO_MEM_WORDS &&
                   fused_away[addr + count + 1])
               count++;
            if (opt_level < 3 || an.ac_live_out[addr + count]) {
               if (count == 0)
                  mano_emit(f, "    xor %s, %s\n", ac32, ac32);
               else
                  mano_emit(f, "    mov %s, %d\n", ac32, count & 0xFFFF);
            }
            emit_fallthrough(f, prog, addr + 1 + count, next_emitted);
         } break;
         case 0x7400: /* CLE */
            if (an.need_e)
               mano_emit(f, "    xor %s, %s\n", e32, e32);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7200: /* CMA (+ fused INC -> neg) */
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               if (opt_level < 3 || an.ac_live_out[addr + 1])
                  mano_emit(f, "    neg %s\n", ac16);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               if (opt_level < 3 || an.ac_live_out[addr])
                  mano_emit(f, "    not %s\n", ac16);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7100: /* CME */
            if (an.need_e)
               mano_emit(f, "    xor %s, 1\n", e32);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7080: /* CIR */
            mano_emit(f, "    bt %s, 0\n    rcr %s, 1\n", e32, ac16);
            o1_set_e_from_cf(f, x64);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7040: /* CIL */
            mano_emit(f, "    bt %s, 0\n    rcl %s, 1\n", e32, ac16);
            o1_set_e_from_cf(f, x64);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0x7020: { /* INC (+ fused INC*) */
            int count = 1;
            while (addr + count < MANO_MEM_WORDS &&
                   fused_away[addr + count])
               count++;
            if (opt_level < 3 || an.ac_live_out[addr + count - 1]) {
               if (count == 1)
                  mano_emit(f, "    inc %s\n", ac16);
               else
                  mano_emit(f, "    add %s, %d\n", ac16, count & 0xFFFF);
            }
            emit_fallthrough(f, prog, addr + count, next_emitted);
         } break;
         case 0x7010: /* SPA */
            mano_emit(f, "    test %s, %s\n", ac16, ac16);
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               emit_target_jmp(f, prog, "js", bun.addr);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               emit_target_jmp(f, prog, "jns", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7008: /* SNA */
            mano_emit(f, "    test %s, %s\n", ac16, ac16);
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               emit_target_jmp(f, prog, "jns", bun.addr);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               emit_target_jmp(f, prog, "js", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7004: /* SZA */
            mano_emit(f, "    test %s, %s\n", ac16, ac16);
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               emit_target_jmp(f, prog, "jnz", bun.addr);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               emit_target_jmp(f, prog, "jz", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7002: /* SZE */
            mano_emit(f, "    test %s, %s\n", e32, e32);
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               emit_target_jmp(f, prog, "jnz", bun.addr);
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               emit_target_jmp(f, prog, "jz", addr + 2);
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0x7001: /* HLT */
            sys_exit(f, x64, 0);
            break;
         default:
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         const char* res = x64 ? "rax" : "eax";
         switch (code) {
         case 0xF800: /* INP */
            ld(f, x64, "eax", ".Lfgi");
            mano_emit(f, "    test eax, eax\n    jne 2f\n");
            ldi(f, x64, ".Linpr", 0);
            sys_read1(f, x64, ".Linpr");
            mano_emit(f, "2:\n");
            ld(f, x64, ac32, ".Linpr");
            ldi(f, x64, ".Lfgi", 0);
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF400: /* OUT */
            if (x64) {
               mano_emit(f, "    mov byte ptr [rip+.Loutr], r12b\n");
            }
            else {
               mano_emit(f, "    mov eax, edi\n    and eax, 255\n"
                            "    mov dword ptr [.Loutr], eax\n");
            }
            sys_write1(f, x64, ".Loutr");
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         case 0xF200: /* SKI */
            ld(f, x64, "eax", ".Lfgi");
            mano_emit(f, "    test eax, eax\n    jne 2f\n");
            ldi(f, x64, ".Linpr", 0);
            sys_read1(f, x64, ".Linpr");
            if (addr + 1 < MANO_MEM_WORDS && fused_away[addr + 1]) {
               ManoInstr bun;
               mano_decode(prog->mem[addr + 1], &bun);
               mano_emit(f, "    cmp %s, 0\n", res);
               emit_target_jmp(f, prog, "jle", bun.addr);
               ldi(f, x64, ".Lfgi", 1);
               mano_emit(f, "2:\n");
               emit_fallthrough(f, prog, addr + 2, next_emitted);
            }
            else {
               mano_emit(f, "    cmp %s, 0\n    jle 3f\n", res);
               ldi(f, x64, ".Lfgi", 1);
               mano_emit(f, "2:\n");
               emit_target_jmp(f, prog, "jmp", addr + 2);
               mano_emit(f, "3:\n");
               emit_fallthrough(f, prog, addr + 1, next_emitted);
            }
            break;
         case 0xF100: /* SKO */
            emit_fallthrough(f, prog, addr + 2, next_emitted);
            break;
         case 0xF080:
         case 0xF040:
            emit_fallthrough(f, prog, addr + 1, next_emitted);
            break;
         default:
            break;
         }
      }
   }

   emit_bad_epilogue(f, x64, badlen);
}

void mano_emit_x86(FILE* f, const ManoProgram* prog, int start_pc, bool x64,
                   int opt_level) {
   if (opt_level <= 0)
      emit_x86_o0(f, prog, start_pc, x64);
   else if (opt_level == 1)
      emit_x86_o1(f, prog, start_pc, x64);
   else
      emit_x86_opt(f, prog, start_pc, x64, opt_level);
}
