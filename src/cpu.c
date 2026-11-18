#define _POSIX_C_SOURCE 200809L

#include "cpu.h"

#include <stdio.h>
#include <string.h>

static int stdio_read(void* ctx) {
   (void)ctx;
   int c = getchar();
   return c == EOF ? -1 : (c & 0xFF);
}

static void stdio_write(void* ctx, int ch) {
   (void)ctx;
   putchar(ch);
   fflush(stdout);
}

void mano_cpu_set_stdio(ManoCPU* cpu) {
   cpu->io.read = stdio_read;
   cpu->io.write = stdio_write;
   cpu->io.ctx = NULL;
}

void mano_cpu_reset(ManoCPU* cpu) {
   const ManoProgram* prog = cpu->prog;
   ManoIO io = cpu->io;
   int start_pc = cpu->start_pc;

   memcpy(cpu->mem, prog->mem, sizeof cpu->mem);
   cpu->ac = 0;
   cpu->pc = (uint16_t)(start_pc & MANO_ADDR_MASK);
   cpu->ir = 0;
   cpu->ar = 0;
   cpu->dr = 0;
   cpu->tr = 0;
   cpu->inpr = 0;
   cpu->outr = 0;
   cpu->e = false;
   cpu->ien = false;
   cpu->fgi = false;
   cpu->halted = false;
   cpu->fault = false;
   cpu->fault_ir = 0;
   cpu->fault_addr = 0;
   cpu->steps = 0;

   cpu->prog = prog;
   cpu->io = io;
   cpu->start_pc = start_pc;
}

void mano_cpu_init(ManoCPU* cpu, const ManoProgram* prog, int start_pc,
                   const ManoIO* io) {
   memset(cpu, 0, sizeof *cpu);
   cpu->prog = prog;
   cpu->start_pc = start_pc & MANO_ADDR_MASK;
   if (io)
      cpu->io = *io;
   mano_cpu_reset(cpu);
}

static inline uint16_t inc_pc(uint16_t pc) {
   return (uint16_t)((pc + 1u) & MANO_ADDR_MASK);
}

int mano_cpu_step(ManoCPU* cpu) {
   uint16_t* mem = cpu->mem;
   uint16_t ar = (uint16_t)(cpu->pc & MANO_ADDR_MASK);
   uint16_t ir = mem[ar];
   uint16_t pc = inc_pc(ar);

   cpu->ar = ar;
   cpu->ir = ir;
   cpu->pc = pc;

   unsigned op = (unsigned)((ir >> 12) & 7u);
   if (op != 7u) {
      unsigned addr = (unsigned)(ir & MANO_ADDR_MASK);
      if (ir & 0x8000u)
         addr = (unsigned)(mem[addr] & MANO_ADDR_MASK);
      uint16_t dr = mem[addr];
      cpu->ar = (uint16_t)addr;
      cpu->dr = dr;

      switch (op) {
      case 0:
         cpu->ac = (uint16_t)(cpu->ac & dr);
         break;
      case 1: {
         unsigned sum = (unsigned)cpu->ac + (unsigned)dr;
         cpu->e = sum > MANO_WORD_MASK;
         cpu->ac = (uint16_t)(sum & MANO_WORD_MASK);
      } break;
      case 2:
         cpu->ac = dr;
         break;
      case 3:
         mem[addr] = cpu->ac;
         break;
      case 4:
         cpu->pc = (uint16_t)addr;
         break;
      case 5:
         mem[addr] = pc;
         cpu->pc = (uint16_t)((addr + 1u) & MANO_ADDR_MASK);
         break;
      case 6:
         dr = (uint16_t)((dr + 1u) & MANO_WORD_MASK);
         cpu->dr = dr;
         mem[addr] = dr;
         if (dr == 0)
            cpu->pc = inc_pc(pc);
         break;
      default:
         break;
      }
   }
   else {
      switch (ir) {
      case 0x7800:
         cpu->ac = 0;
         break;
      case 0x7400:
         cpu->e = false;
         break;
      case 0x7200:
         cpu->ac = (uint16_t)(~cpu->ac);
         break;
      case 0x7100:
         cpu->e = !cpu->e;
         break;
      case 0x7080: {
         bool old_e = cpu->e;
         cpu->e = (cpu->ac & 1u) != 0;
         cpu->ac = (uint16_t)((cpu->ac >> 1) | (old_e ? 0x8000u : 0u));
      } break;
      case 0x7040: {
         bool old_e = cpu->e;
         cpu->e = (cpu->ac & 0x8000u) != 0;
         cpu->ac = (uint16_t)(((cpu->ac << 1) & MANO_WORD_MASK) |
                              (old_e ? 1u : 0u));
      } break;
      case 0x7020:
         cpu->ac = (uint16_t)((cpu->ac + 1u) & MANO_WORD_MASK);
         break;
      case 0x7010:
         if ((cpu->ac & 0x8000u) == 0)
            cpu->pc = inc_pc(pc);
         break;
      case 0x7008:
         if (cpu->ac & 0x8000u)
            cpu->pc = inc_pc(pc);
         break;
      case 0x7004:
         if (cpu->ac == 0)
            cpu->pc = inc_pc(pc);
         break;
      case 0x7002:
         if (!cpu->e)
            cpu->pc = inc_pc(pc);
         break;
      case 0x7001:
         cpu->halted = true;
         break;
      case 0xF800:
         if (!cpu->fgi) {
            int ch = cpu->io.read ? cpu->io.read(cpu->io.ctx) : -1;
            cpu->inpr = (uint16_t)(ch < 0 ? 0 : (ch & 0xFF));
         }
         cpu->ac = cpu->inpr;
         cpu->fgi = false;
         break;
      case 0xF400:
         cpu->outr = (uint16_t)(cpu->ac & 0x00FFu);
         if (cpu->io.write)
            cpu->io.write(cpu->io.ctx, (int)cpu->outr);
         break;
      case 0xF200:
         if (!cpu->fgi) {
            int ch = cpu->io.read ? cpu->io.read(cpu->io.ctx) : -1;
            if (ch >= 0) {
               cpu->inpr = (uint16_t)(ch & 0xFF);
               cpu->fgi = true;
            }
         }
         if (cpu->fgi)
            cpu->pc = inc_pc(pc);
         break;
      case 0xF100:
         cpu->pc = inc_pc(pc);
         break;
      case 0xF080:
         cpu->ien = true;
         break;
      case 0xF040:
         cpu->ien = false;
         break;
      default:
         cpu->fault = true;
         cpu->halted = true;
         cpu->fault_ir = ir;
         cpu->fault_addr = ar;
         cpu->steps++;
         return -1;
      }
   }

   cpu->steps++;
   return 0;
}

int mano_cpu_run(ManoCPU* cpu, unsigned long long maxsteps) {
   uint16_t* mem = cpu->mem;
   uint16_t ac = cpu->ac;
   uint16_t pc = (uint16_t)(cpu->pc & MANO_ADDR_MASK);
   uint16_t ar = cpu->ar;
   uint16_t ir = cpu->ir;
   uint16_t dr = cpu->dr;
   bool e = cpu->e;
   bool ien = cpu->ien;
   bool fgi = cpu->fgi;
   uint16_t inpr = cpu->inpr;
   uint16_t outr = cpu->outr;
   bool halted = cpu->halted;
   unsigned long long steps = cpu->steps;
   int rc = 0;

   while (!halted) {
      if (steps >= maxsteps) {
         rc = 1;
         break;
      }

      ar = pc;
      ir = mem[ar];
      pc = inc_pc(ar);
      steps++;

      unsigned op = (unsigned)((ir >> 12) & 7u);
      if (op != 7u) {
         unsigned addr = (unsigned)(ir & MANO_ADDR_MASK);
         if (ir & 0x8000u)
            addr = (unsigned)(mem[addr] & MANO_ADDR_MASK);
         ar = (uint16_t)addr;
         dr = mem[addr];

         switch (op) {
         case 0:
            ac = (uint16_t)(ac & dr);
            break;
         case 1: {
            unsigned sum = (unsigned)ac + (unsigned)dr;
            e = sum > MANO_WORD_MASK;
            ac = (uint16_t)(sum & MANO_WORD_MASK);
         } break;
         case 2:
            ac = dr;
            break;
         case 3:
            mem[addr] = ac;
            break;
         case 4:
            pc = (uint16_t)addr;
            break;
         case 5:
            mem[addr] = pc;
            pc = (uint16_t)((addr + 1u) & MANO_ADDR_MASK);
            break;
         case 6:
            dr = (uint16_t)((dr + 1u) & MANO_WORD_MASK);
            mem[addr] = dr;
            if (dr == 0)
               pc = inc_pc(pc);
            break;
         default:
            break;
         }
         continue;
      }

      switch (ir) {
      case 0x7800:
         ac = 0;
         break;
      case 0x7400:
         e = false;
         break;
      case 0x7200:
         ac = (uint16_t)(~ac);
         break;
      case 0x7100:
         e = !e;
         break;
      case 0x7080: {
         bool old_e = e;
         e = (ac & 1u) != 0;
         ac = (uint16_t)((ac >> 1) | (old_e ? 0x8000u : 0u));
      } break;
      case 0x7040: {
         bool old_e = e;
         e = (ac & 0x8000u) != 0;
         ac = (uint16_t)(((ac << 1) & MANO_WORD_MASK) | (old_e ? 1u : 0u));
      } break;
      case 0x7020:
         ac = (uint16_t)((ac + 1u) & MANO_WORD_MASK);
         break;
      case 0x7010:
         if ((ac & 0x8000u) == 0)
            pc = inc_pc(pc);
         break;
      case 0x7008:
         if (ac & 0x8000u)
            pc = inc_pc(pc);
         break;
      case 0x7004:
         if (ac == 0)
            pc = inc_pc(pc);
         break;
      case 0x7002:
         if (!e)
            pc = inc_pc(pc);
         break;
      case 0x7001:
         halted = true;
         break;
      case 0xF800:
         if (!fgi) {
            int ch = cpu->io.read ? cpu->io.read(cpu->io.ctx) : -1;
            inpr = (uint16_t)(ch < 0 ? 0 : (ch & 0xFF));
         }
         ac = inpr;
         fgi = false;
         break;
      case 0xF400:
         outr = (uint16_t)(ac & 0x00FFu);
         if (cpu->io.write)
            cpu->io.write(cpu->io.ctx, (int)outr);
         break;
      case 0xF200:
         if (!fgi) {
            int ch = cpu->io.read ? cpu->io.read(cpu->io.ctx) : -1;
            if (ch >= 0) {
               inpr = (uint16_t)(ch & 0xFF);
               fgi = true;
            }
         }
         if (fgi)
            pc = inc_pc(pc);
         break;
      case 0xF100:
         pc = inc_pc(pc);
         break;
      case 0xF080:
         ien = true;
         break;
      case 0xF040:
         ien = false;
         break;
      default:
         cpu->fault = true;
         halted = true;
         cpu->fault_ir = ir;
         cpu->fault_addr = ar;
         rc = -1;
         break;
      }
   }

   cpu->ac = ac;
   cpu->pc = pc;
   cpu->ar = ar;
   cpu->ir = ir;
   cpu->dr = dr;
   cpu->e = e;
   cpu->ien = ien;
   cpu->fgi = fgi;
   cpu->inpr = inpr;
   cpu->outr = outr;
   cpu->halted = halted;
   cpu->steps = steps;
   return rc;
}
