#ifndef MANO_ISA_H
#define MANO_ISA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MANO_KIND_MEM = 0,
               MANO_KIND_REG = 1,
               MANO_KIND_IO = 2 } ManoKind;

typedef struct {
      const char* name;
      ManoKind kind;
      int code;
} ManoMnemonic;

typedef struct {
      ManoKind kind;
      int op;
      int indirect;
      int addr;
} ManoInstr;

extern const ManoMnemonic mano_mnemonics[];
extern const int mano_mnemonic_count;

const ManoMnemonic* mano_mnemonic_find(const char* name);
const ManoMnemonic* mano_mnemonic_find_n(const char* name, size_t len);
int mano_pseudo_id(const char* name, size_t len);
bool mano_decode(uint16_t word, ManoInstr* out);

#endif
