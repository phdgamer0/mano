#ifndef MANO_CPU_H
#define MANO_CPU_H

#include "mano.h"

typedef struct {
      int (*read)(void* ctx);
      void (*write)(void* ctx, int ch);
      void* ctx;
} ManoIO;

typedef struct {
      const ManoProgram* prog;
      uint16_t mem[MANO_MEM_WORDS];
      uint16_t ac, pc, ir, ar, dr, tr;
      uint16_t inpr, outr;
      bool e, ien, fgi;
      bool halted, fault;
      uint16_t fault_ir, fault_addr;
      unsigned long long steps;
      int start_pc;
      ManoIO io;
} ManoCPU;

void mano_cpu_init(ManoCPU* cpu, const ManoProgram* prog, int start_pc, const ManoIO* io);
void mano_cpu_reset(ManoCPU* cpu);
int mano_cpu_step(ManoCPU* cpu);
int mano_cpu_run(ManoCPU* cpu, unsigned long long maxsteps);
void mano_cpu_set_stdio(ManoCPU* cpu);

#endif
