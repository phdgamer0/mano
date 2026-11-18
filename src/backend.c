#define _POSIX_C_SOURCE 200809L

#include "backend.h"

#include "backend_int.h"
#include "isa.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char badmsg_body[] =
    "mano: runtime error: attempted to execute a non-instruction";

static const char hex_chars[] = "0123456789ABCDEF";

void mano_emit(FILE* f, const char* fmt, ...) {
   if (!strchr(fmt, '%')) {
      fputs(fmt, f);
      return;
   }
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
}

void mano_emit_dataseg(FILE* f, const ManoProgram* prog) {
   fputs("\n.section .data\n.balign 16\n.Lmem:\n", f);

   char chunk[8192];
   size_t pos = 0;

   long emitted = 0;
   int zeros = 0;
   int last = prog->last_word;
   for (int i = 0; i <= last; i++) {
      uint16_t w = prog->mem[i];
      if (w == 0) {
         zeros++;
         continue;
      }
      if (zeros) {
         if (pos) {
            fwrite(chunk, 1, pos, f);
            pos = 0;
         }
         fprintf(f, "    .zero %d\n", zeros * 2);
         emitted += (long)zeros * 2;
         zeros = 0;
      }
      if (pos + 24 > sizeof chunk) {
         fwrite(chunk, 1, pos, f);
         pos = 0;
      }
      memcpy(chunk + pos, "    .short 0x", 13);
      pos += 13;
      chunk[pos++] = hex_chars[(w >> 12) & 0xFu];
      chunk[pos++] = hex_chars[(w >> 8) & 0xFu];
      chunk[pos++] = hex_chars[(w >> 4) & 0xFu];
      chunk[pos++] = hex_chars[w & 0xFu];
      chunk[pos++] = '\n';
      emitted += 2;
   }
   if (pos)
      fwrite(chunk, 1, pos, f);
   if (zeros) {
      fprintf(f, "    .zero %d\n", zeros * 2);
      emitted += (long)zeros * 2;
   }
   long total = (long)MANO_MEM_WORDS * 2;
   if (emitted < total)
      fprintf(f, "    .zero %ld\n", total - emitted);
}

int mano_emit_badmsg(FILE* f) {
   mano_emit(f, "\n.section .rodata\n.Lbadmsg:\n"
                "    .ascii \"%s\"\n"
                "    .byte 10\n",
             badmsg_body);
   return (int)sizeof badmsg_body;
}

void mano_emit_jumptable(FILE* f, const ManoProgram* prog,
                         const char* directive) {
   int entry = strcmp(directive, "quad") == 0 ? 8 : 4;
   size_t dlen = strlen(directive);

   fprintf(f, "\n.section .data\n.balign %d\n.Ljtab:\n", entry);

   char chunk[16384];
   size_t pos = 0;
   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      if (pos + 32 > sizeof chunk) {
         fwrite(chunk, 1, pos, f);
         pos = 0;
      }
      memcpy(chunk + pos, "    .", 5);
      pos += 5;
      memcpy(chunk + pos, directive, dlen);
      pos += dlen;
      if (prog->is_code[a]) {
         memcpy(chunk + pos, " .Lx", 4);
         pos += 4;
         chunk[pos++] = hex_chars[(a >> 8) & 0xFu];
         chunk[pos++] = hex_chars[(a >> 4) & 0xFu];
         chunk[pos++] = hex_chars[a & 0xFu];
         chunk[pos++] = '\n';
      }
      else {
         memcpy(chunk + pos, " .Lbad\n", 7);
         pos += 7;
      }
   }
   if (pos)
      fwrite(chunk, 1, pos, f);
}

static void compute_ac_liveness(const ManoProgram* prog, ManoAnalysis* out) {
   uint8_t use_ac[MANO_MEM_WORDS];
   uint8_t def_ac[MANO_MEM_WORDS];
   uint8_t live_in[MANO_MEM_WORDS];
   int succ0[MANO_MEM_WORDS];
   int succ1[MANO_MEM_WORDS];

   memset(use_ac, 0, sizeof use_ac);
   memset(def_ac, 0, sizeof def_ac);
   memset(live_in, 0, sizeof live_in);

   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      succ0[a] = -1;
      succ1[a] = -1;
      if (!out->reachable[a])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[a], &ins))
         continue;

      if (ins.kind == MANO_KIND_MEM) {
         switch (ins.op) {
         case 0: /* AND */
         case 1: /* ADD */
         case 3: /* STA */
            use_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            break;
         case 2: /* LDA */
            def_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            break;
         case 4: /* BUN */
            if (!ins.indirect)
               succ0[a] = ins.addr & MANO_ADDR_MASK;
            break;
         case 5: /* BSA */
            if (!ins.indirect)
               succ0[a] = (ins.addr + 1) & MANO_ADDR_MASK;
            break;
         case 6: /* ISZ */
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            succ1[a] = (a + 2) & MANO_ADDR_MASK;
            break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         switch (code) {
         case 0x7800: /* CLA */
            def_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            break;
         case 0x7200: /* CMA */
         case 0x7080: /* CIR */
         case 0x7040: /* CIL */
         case 0x7020: /* INC */
            use_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            break;
         case 0x7010: /* SPA */
         case 0x7008: /* SNA */
         case 0x7004: /* SZA */
            use_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            succ1[a] = (a + 2) & MANO_ADDR_MASK;
            break;
         case 0x7002: /* SZE */
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            succ1[a] = (a + 2) & MANO_ADDR_MASK;
            break;
         case 0x7001: /* HLT */
            break;
         default: /* CLE, CME */
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            break;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0xF800) { /* INP */
            def_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
         }
         else if (code == 0xF400) { /* OUT */
            use_ac[a] = 1;
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
         }
         else if (code == 0xF100) { /* SKO */
            succ0[a] = (a + 2) & MANO_ADDR_MASK;
         }
         else if (code == 0xF200) { /* SKI */
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
            succ1[a] = (a + 2) & MANO_ADDR_MASK;
         }
         else { /* ION, IOF */
            succ0[a] = (a + 1) & MANO_ADDR_MASK;
         }
      }
   }

   /* Conservative if indirect jumps exist: treat AC as live everywhere. */
   if (out->has_indirect_jmp) {
      for (int a = 0; a < MANO_MEM_WORDS; a++)
         out->ac_live_out[a] = 1;
      return;
   }

   bool changed = true;
   while (changed) {
      changed = false;
      for (int a = MANO_MEM_WORDS - 1; a >= 0; a--) {
         if (!out->reachable[a])
            continue;
         uint8_t lo = 0;
         if (succ0[a] >= 0 && out->reachable[succ0[a]] && live_in[succ0[a]])
            lo = 1;
         if (succ1[a] >= 0 && out->reachable[succ1[a]] && live_in[succ1[a]])
            lo = 1;
         uint8_t li = use_ac[a] || (lo && !def_ac[a]);
         if (lo != out->ac_live_out[a] || li != live_in[a]) {
            out->ac_live_out[a] = lo;
            live_in[a] = li;
            changed = true;
         }
      }
   }
}

void mano_analyze(const ManoProgram* prog, int start_pc, int opt_level,
                  int max_promoted, ManoAnalysis* out) {
   memset(out, 0, sizeof *out);
   for (int i = 0; i < MANO_MEM_WORDS; i++)
      out->slot_reg[i] = -1;

   if (max_promoted > MANO_MAX_PROMOTED)
      max_promoted = MANO_MAX_PROMOTED;
   if (max_promoted < 0)
      max_promoted = 0;

   int queue[MANO_MEM_WORDS];
   int qhead = 0, qtail = 0;
   uint8_t enqueued[MANO_MEM_WORDS];
   memset(enqueued, 0, sizeof enqueued);

   int entry = start_pc & MANO_ADDR_MASK;
   out->is_target[entry] = 1;
   if (prog->is_code[entry]) {
      enqueued[entry] = 1;
      queue[qtail++] = entry;
   }

   bool expanded_all = false;
   while (qhead < qtail) {
      int addr = queue[qhead++];
      if (!prog->is_code[addr])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[addr], &ins))
         continue;
      out->reachable[addr] = 1;

      int succs[2];
      int nsucc = 0;

      if (ins.kind == MANO_KIND_MEM) {
         if (ins.indirect) {
            if (ins.op == 4 || ins.op == 5)
               out->has_indirect_jmp = true;
            else
               out->has_indirect_mem = true;
         }
         switch (ins.op) {
         case 0:
         case 1:
         case 2:
         case 3:
            succs[nsucc++] = (addr + 1) & MANO_ADDR_MASK;
            break;
         case 4: /* BUN */
            if (!ins.indirect) {
               int tgt = ins.addr & MANO_ADDR_MASK;
               out->is_target[tgt] = 1;
               succs[nsucc++] = tgt;
            }
            break;
         case 5: /* BSA */
            if (!ins.indirect) {
               int tgt = (ins.addr + 1) & MANO_ADDR_MASK;
               int ret = (addr + 1) & MANO_ADDR_MASK;
               out->is_target[tgt] = 1;
               out->is_target[ret] = 1;
               succs[nsucc++] = tgt;
               succs[nsucc++] = ret;
            }
            break;
         case 6: { /* ISZ */
            int fall = (addr + 1) & MANO_ADDR_MASK;
            int skip = (addr + 2) & MANO_ADDR_MASK;
            out->is_target[skip] = 1;
            succs[nsucc++] = fall;
            succs[nsucc++] = skip;
         } break;
         default:
            break;
         }
      }
      else if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0x7001) {
            /* HLT */
         }
         else if (code == 0x7010 || code == 0x7008 || code == 0x7004 ||
                  code == 0x7002) {
            int fall = (addr + 1) & MANO_ADDR_MASK;
            int skip = (addr + 2) & MANO_ADDR_MASK;
            out->is_target[skip] = 1;
            succs[nsucc++] = fall;
            succs[nsucc++] = skip;
         }
         else {
            succs[nsucc++] = (addr + 1) & MANO_ADDR_MASK;
         }
      }
      else {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0xF100) { /* SKO always skips */
            int skip = (addr + 2) & MANO_ADDR_MASK;
            out->is_target[skip] = 1;
            succs[nsucc++] = skip;
         }
         else if (code == 0xF200) { /* SKI */
            int fall = (addr + 1) & MANO_ADDR_MASK;
            int skip = (addr + 2) & MANO_ADDR_MASK;
            out->is_target[skip] = 1;
            succs[nsucc++] = fall;
            succs[nsucc++] = skip;
         }
         else {
            succs[nsucc++] = (addr + 1) & MANO_ADDR_MASK;
         }
      }

      for (int s = 0; s < nsucc; s++) {
         int nxt = succs[s];
         if (nxt != addr + 1)
            out->is_target[nxt] = 1;
         if (prog->is_code[nxt] && !enqueued[nxt]) {
            enqueued[nxt] = 1;
            queue[qtail++] = nxt;
         }
      }

      if (out->has_indirect_jmp && !expanded_all) {
         expanded_all = true;
         for (int a = 0; a < MANO_MEM_WORDS; a++) {
            if (prog->is_code[a]) {
               out->is_target[a] = 1;
               if (!enqueued[a]) {
                  enqueued[a] = 1;
                  queue[qtail++] = a;
               }
            }
         }
      }
   }

   if (opt_level < 3) {
      for (int a = 0; a < MANO_MEM_WORDS; a++) {
         if (prog->is_code[a])
            out->reachable[a] = 1;
         out->ac_live_out[a] = 1;
      }
   }
   else {
      compute_ac_liveness(prog, out);
   }

   out->need_jtab = (opt_level < 2) || out->has_indirect_jmp;
   out->need_e = (opt_level < 3);
   out->need_io_state = (opt_level < 2);
   out->need_mem = (opt_level < 3) || out->has_indirect_mem ||
                   out->has_indirect_jmp;

   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      if (!out->reachable[a])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[a], &ins))
         continue;
      if (ins.kind == MANO_KIND_REG) {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0x7002 || code == 0x7080 || code == 0x7040)
            out->need_e = true;
      }
      else if (ins.kind == MANO_KIND_IO) {
         int code = mano_mnemonics[ins.op].code;
         if (code == 0xF800 || code == 0xF400 || code == 0xF200)
            out->need_io_state = true;
      }
   }

   if (opt_level < 3)
      return;

   /* Identify backward-branch loop headers for alignment & weighting. */
   uint8_t loop_depth[MANO_MEM_WORDS];
   memset(loop_depth, 0, sizeof loop_depth);
   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      if (!out->reachable[a])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[a], &ins))
         continue;
      if (ins.kind == MANO_KIND_MEM && ins.op == 4 && !ins.indirect) {
         int tgt = ins.addr & MANO_ADDR_MASK;
         if (tgt <= a) {
            out->is_loop_header[tgt] = 1;
            for (int k = tgt; k <= a; k++) {
               if (loop_depth[k] < 6)
                  loop_depth[k]++;
            }
         }
      }
   }

   if (out->has_indirect_mem)
      return;

   uint8_t no_promote[MANO_MEM_WORDS];
   uint8_t has_isz[MANO_MEM_WORDS];
   memset(no_promote, 0, sizeof no_promote);
   memset(has_isz, 0, sizeof has_isz);

   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      if (!out->reachable[a])
         continue;
      ManoInstr ins;
      if (!mano_decode(prog->mem[a], &ins))
         continue;
      if (ins.kind != MANO_KIND_MEM)
         continue;
      int target = ins.addr & MANO_ADDR_MASK;
      if (ins.op == 4 || ins.op == 5) {
         no_promote[target] = 1;
         if (ins.op == 5)
            out->written[target] = 1;
      }
      else {
         uint32_t w = 1u << (loop_depth[a] * 3u);
         out->access_cnt[target] += w;
         if (ins.op == 0 || ins.op == 1 || ins.op == 2)
            out->is_read[target] = 1;
         if (ins.op == 3 || ins.op == 6)
            out->written[target] = 1;
         if (ins.op == 6)
            has_isz[target] = 1;
      }
   }

   for (int a = 0; a < MANO_MEM_WORDS; a++) {
      if (!out->written[a]) {
         out->is_const[a] = 1;
      }
      else if (!out->is_read[a] && !has_isz[a] && !no_promote[a] &&
               !prog->is_code[a]) {
         out->is_dead_store[a] = 1;
      }
   }

   for (int slot = 0; slot < max_promoted; slot++) {
      int best = -1;
      uint32_t best_cnt = 0;
      for (int a = 0; a < MANO_MEM_WORDS; a++) {
         if (!out->written[a] || out->is_dead_store[a] || no_promote[a] ||
             prog->is_code[a])
            continue;
         if (out->slot_reg[a] >= 0)
            continue;
         if (out->access_cnt[a] > best_cnt) {
            best_cnt = out->access_cnt[a];
            best = a;
         }
      }
      if (best < 0)
         break;
      out->slot_reg[best] = (int8_t)slot;
      out->promoted_addr[slot] = best;
      if (!out->is_read[best] && has_isz[best])
         out->is_counter[best] = 1;
      out->num_promoted = slot + 1;
   }

   if (!out->need_mem) {
      for (int a = 0; a < MANO_MEM_WORDS; a++) {
         if (!out->reachable[a])
            continue;
         ManoInstr ins;
         if (!mano_decode(prog->mem[a], &ins))
            continue;
         if (ins.kind != MANO_KIND_MEM)
            continue;
         int target = ins.addr & MANO_ADDR_MASK;
         if (ins.op == 5) {
            out->need_mem = true;
            break;
         }
         if (ins.op != 4 && !out->is_const[target] &&
             !out->is_dead_store[target] && out->slot_reg[target] < 0) {
            out->need_mem = true;
            break;
         }
      }
   }
}

static const struct {
      const char* alias;
      ManoArch arch;
} arch_names[] = {
    {"amd64", MANO_ARCH_AMD64},
    {"x86_64", MANO_ARCH_AMD64},
    {"x86-64", MANO_ARCH_AMD64},
    {"x64", MANO_ARCH_AMD64},
    {"i386", MANO_ARCH_I386},
    {"x86_32", MANO_ARCH_I386},
    {"x86-32", MANO_ARCH_I386},
    {"x86", MANO_ARCH_I386},
    {"ia32", MANO_ARCH_I386},
    {"arm64", MANO_ARCH_ARM64},
    {"aarch64", MANO_ARCH_ARM64},
    {"arm32", MANO_ARCH_ARM32},
    {"arm", MANO_ARCH_ARM32},
    {"aarch32", MANO_ARCH_ARM32},
    {"armv7", MANO_ARCH_ARM32},
    {"armhf", MANO_ARCH_ARM32},
};

bool mano_arch_parse(const char* name, ManoArch* out) {
   for (size_t i = 0; i < sizeof arch_names / sizeof arch_names[0]; i++) {
      if (str_eq_ci(name, arch_names[i].alias)) {
         *out = arch_names[i].arch;
         return true;
      }
   }
   return false;
}

const char* mano_arch_name(ManoArch arch) {
   switch (arch) {
   case MANO_ARCH_AMD64:
      return "amd64";
   case MANO_ARCH_I386:
      return "i386";
   case MANO_ARCH_ARM64:
      return "arm64";
   case MANO_ARCH_ARM32:
      return "arm32";
   }
   return "amd64";
}

const char* mano_arch_compiler(ManoArch arch) {
   switch (arch) {
   case MANO_ARCH_AMD64:
      return "cc -nostdlib -static -no-pie";
   case MANO_ARCH_I386:
      return "cc -m32 -nostdlib -static -no-pie";
   case MANO_ARCH_ARM64:
      return "aarch64-linux-gnu-gcc -nostdlib -static";
   case MANO_ARCH_ARM32:
      return "arm-linux-gnueabihf-gcc -nostdlib -static";
   }
   return "cc -nostdlib -static -no-pie";
}

void mano_backend_emit(FILE* f, const ManoProgram* prog, int start_pc,
                       ManoArch arch, int opt_level) {
   if (opt_level < 0)
      opt_level = 0;
   if (opt_level > 3)
      opt_level = 3;

   switch (arch) {
   case MANO_ARCH_AMD64:
      mano_emit_x86(f, prog, start_pc, true, opt_level);
      break;
   case MANO_ARCH_I386:
      mano_emit_x86(f, prog, start_pc, false, opt_level);
      break;
   case MANO_ARCH_ARM64:
      mano_emit_arm64(f, prog, start_pc, opt_level);
      break;
   case MANO_ARCH_ARM32:
      mano_emit_arm32(f, prog, start_pc, opt_level);
      break;
   }
}

int mano_backend_write(const ManoProgram* prog, const char* path,
                       int start_pc, ManoArch arch, int opt_level) {
   FILE* f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "mano: cannot write '%s'\n", path);
      return -1;
   }

   char io_buf[65536];
   setvbuf(f, io_buf, _IOFBF, sizeof io_buf);

   mano_backend_emit(f, prog, start_pc, arch, opt_level);

   bool ok = (fflush(f) == 0) && (ferror(f) == 0);
   if (fclose(f) != 0)
      ok = false;
   if (!ok) {
      fprintf(stderr, "mano: error writing '%s'\n", path);
      return -1;
   }
   return 0;
}
