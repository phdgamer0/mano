#ifndef MANO_BACKEND_INT_H
#define MANO_BACKEND_INT_H

#include "mano.h"
#include "util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MANO_MAX_PROMOTED 6

typedef struct {
      bool has_indirect_mem;
      bool has_indirect_jmp;
      bool need_jtab;
      bool need_e;
      bool need_io_state;
      bool need_mem;

      uint8_t is_target[MANO_MEM_WORDS];
      uint8_t is_loop_header[MANO_MEM_WORDS];
      uint8_t reachable[MANO_MEM_WORDS];
      uint8_t written[MANO_MEM_WORDS];
      uint8_t is_read[MANO_MEM_WORDS];
      uint8_t is_const[MANO_MEM_WORDS];
      uint8_t is_dead_store[MANO_MEM_WORDS];
      uint8_t is_counter[MANO_MEM_WORDS];
      uint8_t ac_live_out[MANO_MEM_WORDS];
      uint32_t access_cnt[MANO_MEM_WORDS];

      int8_t slot_reg[MANO_MEM_WORDS];
      int promoted_addr[MANO_MAX_PROMOTED];
      int num_promoted;
} ManoAnalysis;

void mano_emit(FILE* f, const char* fmt, ...) MANO_PRINTF(2, 3);

void mano_emit_dataseg(FILE* f, const ManoProgram* prog);
int mano_emit_badmsg(FILE* f);
void mano_emit_jumptable(FILE* f, const ManoProgram* prog,
                         const char* directive);

void mano_analyze(const ManoProgram* prog, int start_pc, int opt_level,
                  int max_promoted, ManoAnalysis* out);

void mano_emit_x86(FILE* f, const ManoProgram* prog, int start_pc, bool x64,
                   int opt_level);
void mano_emit_arm64(FILE* f, const ManoProgram* prog, int start_pc,
                     int opt_level);
void mano_emit_arm32(FILE* f, const ManoProgram* prog, int start_pc,
                     int opt_level);

#endif
