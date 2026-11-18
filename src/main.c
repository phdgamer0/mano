#define _POSIX_C_SOURCE 200809L

#include "asm.h"
#include "backend.h"
#include "cpu.h"
#include "mano.h"
#include "tui.h"
#include "util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MANO_VERSION
#define MANO_VERSION "dev"
#endif

static void usage(FILE* out) {
   fprintf(
       out,
       "mano - assembler, simulator and multi-target code generator for the\n"
       "       Morris Mano basic computer (Computer System Architecture, 3rd "
       "ed., ch. 5)\n"
       "\n"
       "usage: mano <command> [options] <file.asm>\n"
       "\n"
       "commands:\n"
       "  asm     assemble and print the listing plus symbol table\n"
       "  run     assemble and execute on the simulated machine\n"
       "  debug   assemble and open the full-screen debugger\n"
       "  x86     compile to freestanding Linux assembly (GNU syntax)\n"
       "\n"
       "options:\n"
       "  --trace          run: print every instruction\n"
       "  --max N          run: step limit (default 1000000)\n"
       "  --pc ADDR        start PC in hex (default: first instruction)\n"
       "  --target ARCH    x86: amd64 | i386 | arm64 | arm32 (default: amd64)\n"
       "  -O0..-O3         x86: optimization level (0=micro-ops .. 3=native, default: 0)\n"
       "  -o FILE          x86: output file (default: <input>[.<arch>].s)\n"
       "  -h, --help       show this help\n"
       "  -V, --version    show the version\n"
       "\n"
       "source syntax:\n"
       "  ORG 100          set the location counter (hex)\n"
       "  LABEL, DEC 12    label with a decimal word\n"
       "  LABEL, HEX 0A3F  label with a hex word\n"
       "  LDA I X          indirect addressing\n"
       "  '/' or ';'       start a comment\n"
       "  END              end of source\n"
       "\n"
       "debugger keys:\n"
       "  s step   r/space run-pause   R reset    b breakpoint at PC\n"
       "  a toggle native asm split    t target   o opt level (-O0..-O3)\n"
       "  i input  [ ] memory page     m memory at AR\n"
       "  arrows/PgUp/PgDn scroll source   f follow PC   c clear output\n"
       "  ? help   q quit\n");
}

static bool parse_hex_arg(const char* s, int* out) {
   if (!s || !*s)
      return false;
   const char* p = s;
   if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
      p += 2;
   char* end = NULL;
   errno = 0;
   long v = strtol(p, &end, 16);
   if (errno == ERANGE || !end || *end != '\0' || v < 0 || v > 0xFFFF)
      return false;
   *out = (int)v;
   return true;
}

static bool parse_count_arg(const char* s, long* out) {
   if (!s || !*s)
      return false;
   char* end = NULL;
   errno = 0;
   long v = strtol(s, &end, 10);
   if (errno == ERANGE || !end || *end != '\0' || v < 0)
      return false;
   *out = v;
   return true;
}

static void print_regs(FILE* f, const ManoCPU* cpu) {
   fprintf(f,
           "AC=%04X E=%d I=%d PC=%03X IR=%04X AR=%03X DR=%04X TR=%04X "
           "FGI=%d INPR=%02X OUTR=%02X\n",
           (unsigned)cpu->ac, cpu->e ? 1 : 0, cpu->ien ? 1 : 0,
           (unsigned)cpu->pc, (unsigned)cpu->ir, (unsigned)cpu->ar,
           (unsigned)cpu->dr, (unsigned)cpu->tr, cpu->fgi ? 1 : 0,
           (unsigned)cpu->inpr, (unsigned)cpu->outr);
}

static void print_trace(const ManoProgram* prog, const ManoCPU* cpu, int addr,
                        uint16_t word) {
   const char* src = "";
   int32_t li = prog->line_of[addr];
   if (li >= 0 && (size_t)li < prog->nlines)
      src = prog->lines[li].text;
   fprintf(stderr, "%03X  %04X  %-30.30s AC=%04X E=%d I=%d PC=%03X\n", addr,
           (unsigned)word, src, (unsigned)cpu->ac, cpu->e ? 1 : 0,
           cpu->ien ? 1 : 0, (unsigned)cpu->pc);
}

static void print_summary(const ManoCPU* cpu, FILE* f) {
   fprintf(f, "\n[halted after %llu instruction%s]\n", cpu->steps,
           cpu->steps == 1 ? "" : "s");
   print_regs(f, cpu);

   fprintf(f, "memory (nonzero):\n");
   int rows = 0;
   for (int base = 0; base < MANO_MEM_WORDS; base += 8) {
      bool any = false;
      for (int j = 0; j < 8; j++)
         if (cpu->mem[base + j]) {
            any = true;
            break;
         }
      if (!any)
         continue;
      if (rows++ == 64) {
         fprintf(f, "...\n");
         break;
      }
      fprintf(f, "%03X:", base);
      for (int j = 0; j < 8; j++)
         fprintf(f, " %04X", (unsigned)cpu->mem[base + j]);
      fputc('\n', f);
   }
}

static int do_run(const ManoProgram* prog, bool trace, long maxsteps,
                  int start_pc) {
   ManoCPU cpu;
   mano_cpu_init(&cpu, prog, start_pc, NULL);
   mano_cpu_set_stdio(&cpu);

   if (!trace) {
      int r = mano_cpu_run(&cpu, (unsigned long long)maxsteps);
      if (r > 0) {
         fprintf(stderr, "mano: stopped after %llu instructions (step limit)\n",
                 cpu.steps);
         print_regs(stderr, &cpu);
         return 1;
      }
      if (r < 0) {
         fprintf(stderr, "mano: runtime error: invalid instruction %04X at %03X\n",
                 (unsigned)cpu.fault_ir, (unsigned)cpu.fault_addr);
         return 1;
      }
      print_summary(&cpu, stderr);
      return 0;
   }

   while (!cpu.halted) {
      if ((long)cpu.steps >= maxsteps) {
         fprintf(stderr, "mano: stopped after %llu instructions (step limit)\n",
                 cpu.steps);
         print_regs(stderr, &cpu);
         return 1;
      }
      int addr = (int)cpu.pc;
      uint16_t word = cpu.mem[addr];
      if (mano_cpu_step(&cpu) < 0) {
         fprintf(stderr, "mano: runtime error: invalid instruction %04X at %03X\n",
                 (unsigned)cpu.fault_ir, (unsigned)cpu.fault_addr);
         return 1;
      }
      print_trace(prog, &cpu, addr, word);
   }

   fputc('\n', stderr);
   print_summary(&cpu, stderr);
   return 0;
}

static char* default_outname(const char* input, ManoArch arch) {
   size_t n = strlen(input);
   size_t base = n;
   if (n > 4 && strcmp(input + n - 4, ".asm") == 0)
      base = n - 4;
   const char* tag = arch == MANO_ARCH_AMD64 ? "" : mano_arch_name(arch);
   size_t taglen = strlen(tag);
   char* out = xmalloc(base + (taglen ? taglen + 1 : 0) + 3);
   memcpy(out, input, base);
   size_t pos = base;
   if (taglen) {
      out[pos++] = '.';
      memcpy(out + pos, tag, taglen);
      pos += taglen;
   }
   memcpy(out + pos, ".s", 3);
   return out;
}

static void print_build_hint(const char* sfile, ManoArch arch) {
   size_t n = strlen(sfile);
   char* bin = xmalloc(n + 1);
   str_copy(bin, n + 1, sfile);
   if (n > 2 && strcmp(bin + n - 2, ".s") == 0)
      bin[n - 2] = '\0';
   fprintf(stderr, "wrote %s\n  build: %s %s -o %s\n", sfile,
           mano_arch_compiler(arch), sfile, bin);
   free(bin);
}

int main(int argc, char** argv) {
   if (argc < 2) {
      usage(stderr);
      return 2;
   }
   if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
       strcmp(argv[1], "help") == 0) {
      usage(stdout);
      return 0;
   }
   if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
      printf("mano %s\n", MANO_VERSION);
      return 0;
   }

   const char* cmd = argv[1];
   if (strcmp(cmd, "asm") == 0 || strcmp(cmd, "run") == 0 ||
       strcmp(cmd, "debug") == 0 || strcmp(cmd, "x86") == 0 ||
       strcmp(cmd, "-S") == 0) {
      if (strcmp(cmd, "-S") == 0)
         cmd = "x86";
      argv++;
      argc--;
   }
   else {
      cmd = "run";
   }

   const char* file = NULL;
   const char* outfile = NULL;
   ManoArch target = MANO_ARCH_AMD64;
   int opt_level = 0;
   bool trace = false;
   long maxsteps = 1000000;
   int start_pc = -1;

   for (int i = 1; i < argc; i++) {
      const char* arg = argv[i];
      if (strcmp(arg, "--trace") == 0 || strcmp(arg, "-t") == 0) {
         trace = true;
      }
      else if (strcmp(arg, "--max") == 0 && i + 1 < argc) {
         if (!parse_count_arg(argv[++i], &maxsteps)) {
            fprintf(stderr, "mano: invalid --max value\n");
            return 2;
         }
      }
      else if (strcmp(arg, "--pc") == 0 && i + 1 < argc) {
         if (!parse_hex_arg(argv[++i], &start_pc)) {
            fprintf(stderr, "mano: invalid --pc value\n");
            return 2;
         }
      }
      else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
         if (!mano_arch_parse(argv[++i], &target)) {
            fprintf(stderr,
                    "mano: unknown target '%s' "
                    "(use amd64, i386, arm64 or arm32)\n",
                    argv[i]);
            return 2;
         }
      }
      else if (strcmp(arg, "-o") == 0 && i + 1 < argc) {
         outfile = argv[++i];
      }
      else if (arg[0] == '-' && arg[1] == 'O') {
         if (arg[2] >= '0' && arg[2] <= '3' && arg[3] == '\0') {
            opt_level = arg[2] - '0';
         }
         else {
            fprintf(stderr,
                    "mano: invalid optimization level '%s' (use -O0..-O3)\n", arg);
            return 2;
         }
      }
      else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
         usage(stdout);
         return 0;
      }
      else if (arg[0] == '-' && arg[1] != '\0') {
         fprintf(stderr, "mano: unknown option '%s'\n", arg);
         return 2;
      }
      else if (!file) {
         file = arg;
      }
      else {
         fprintf(stderr, "mano: unexpected argument '%s'\n", arg);
         return 2;
      }
   }

   if (!file) {
      fprintf(stderr, "mano: no source file given\n");
      return 2;
   }

   if (strcmp(cmd, "debug") == 0 &&
       (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
      fprintf(stderr, "mano: debug needs an interactive terminal "
                      "(use 'mano run --trace' instead)\n");
      return 2;
   }

   ManoProgram* prog = xcalloc(1, sizeof *prog);
   mano_program_init(prog);

   int rc = 0;
   if (mano_assemble_file(file, prog) < 0) {
      rc = 1;
      goto out;
   }

   if (strcmp(cmd, "asm") == 0) {
      mano_print_listing(prog, stdout);
      mano_print_symbols(prog, stdout);
   }
   else if (strcmp(cmd, "run") == 0) {
      int pc = start_pc < 0 ? prog->first_code : start_pc;
      rc = do_run(prog, trace, maxsteps, pc);
   }
   else if (strcmp(cmd, "debug") == 0) {
      int pc = start_pc < 0 ? prog->first_code : start_pc;
      rc = mano_tui_run(prog, pc, target, opt_level);
   }
   else if (strcmp(cmd, "x86") == 0) {
      int pc = start_pc < 0 ? prog->first_code : start_pc;
      char* generated = NULL;
      const char* target_path = outfile;
      if (!target_path) {
         generated = default_outname(file, target);
         target_path = generated;
      }
      if (mano_backend_write(prog, target_path, pc, target, opt_level) < 0) {
         rc = 1;
      }
      else {
         print_build_hint(target_path, target);
      }
      free(generated);
   }
   else {
      fprintf(stderr, "mano: unknown command '%s'\n", cmd);
      usage(stderr);
      rc = 2;
   }

out:
   mano_program_free(prog);
   free(prog);
   return rc;
}
