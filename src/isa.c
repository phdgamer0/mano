#define _POSIX_C_SOURCE 200809L

#include "isa.h"

#include "mano.h"

#include <stdint.h>
#include <string.h>

const ManoMnemonic mano_mnemonics[] = {
    {"AND", MANO_KIND_MEM, 0},
    {"ADD", MANO_KIND_MEM, 1},
    {"LDA", MANO_KIND_MEM, 2},
    {"STA", MANO_KIND_MEM, 3},
    {"BUN", MANO_KIND_MEM, 4},
    {"BSA", MANO_KIND_MEM, 5},
    {"ISZ", MANO_KIND_MEM, 6},
    {"CLA", MANO_KIND_REG, 0x7800},
    {"CLE", MANO_KIND_REG, 0x7400},
    {"CMA", MANO_KIND_REG, 0x7200},
    {"CME", MANO_KIND_REG, 0x7100},
    {"CIR", MANO_KIND_REG, 0x7080},
    {"CIL", MANO_KIND_REG, 0x7040},
    {"INC", MANO_KIND_REG, 0x7020},
    {"SPA", MANO_KIND_REG, 0x7010},
    {"SNA", MANO_KIND_REG, 0x7008},
    {"SZA", MANO_KIND_REG, 0x7004},
    {"SZE", MANO_KIND_REG, 0x7002},
    {"HLT", MANO_KIND_REG, 0x7001},
    {"INP", MANO_KIND_IO, 0xF800},
    {"OUT", MANO_KIND_IO, 0xF400},
    {"SKI", MANO_KIND_IO, 0xF200},
    {"SKO", MANO_KIND_IO, 0xF100},
    {"ION", MANO_KIND_IO, 0xF080},
    {"IOF", MANO_KIND_IO, 0xF040},
};

const int mano_mnemonic_count =
    (int)(sizeof mano_mnemonics / sizeof mano_mnemonics[0]);

#define PACK3(a, b, c)                     \
   (((uint32_t)(unsigned char)(a) << 16) | \
    ((uint32_t)(unsigned char)(b) << 8) | (uint32_t)(unsigned char)(c))

static inline unsigned char upper_ascii(unsigned char c) {
   return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32u) : c;
}

const ManoMnemonic* mano_mnemonic_find_n(const char* name, size_t len) {
   if (!name || len != 3)
      return NULL;
   uint32_t k = PACK3(upper_ascii((unsigned char)name[0]),
                      upper_ascii((unsigned char)name[1]),
                      upper_ascii((unsigned char)name[2]));
   switch (k) {
   case PACK3('A', 'N', 'D'):
      return &mano_mnemonics[0];
   case PACK3('A', 'D', 'D'):
      return &mano_mnemonics[1];
   case PACK3('L', 'D', 'A'):
      return &mano_mnemonics[2];
   case PACK3('S', 'T', 'A'):
      return &mano_mnemonics[3];
   case PACK3('B', 'U', 'N'):
      return &mano_mnemonics[4];
   case PACK3('B', 'S', 'A'):
      return &mano_mnemonics[5];
   case PACK3('I', 'S', 'Z'):
      return &mano_mnemonics[6];
   case PACK3('C', 'L', 'A'):
      return &mano_mnemonics[7];
   case PACK3('C', 'L', 'E'):
      return &mano_mnemonics[8];
   case PACK3('C', 'M', 'A'):
      return &mano_mnemonics[9];
   case PACK3('C', 'M', 'E'):
      return &mano_mnemonics[10];
   case PACK3('C', 'I', 'R'):
      return &mano_mnemonics[11];
   case PACK3('C', 'I', 'L'):
      return &mano_mnemonics[12];
   case PACK3('I', 'N', 'C'):
      return &mano_mnemonics[13];
   case PACK3('S', 'P', 'A'):
      return &mano_mnemonics[14];
   case PACK3('S', 'N', 'A'):
      return &mano_mnemonics[15];
   case PACK3('S', 'Z', 'A'):
      return &mano_mnemonics[16];
   case PACK3('S', 'Z', 'E'):
      return &mano_mnemonics[17];
   case PACK3('H', 'L', 'T'):
      return &mano_mnemonics[18];
   case PACK3('I', 'N', 'P'):
      return &mano_mnemonics[19];
   case PACK3('O', 'U', 'T'):
      return &mano_mnemonics[20];
   case PACK3('S', 'K', 'I'):
      return &mano_mnemonics[21];
   case PACK3('S', 'K', 'O'):
      return &mano_mnemonics[22];
   case PACK3('I', 'O', 'N'):
      return &mano_mnemonics[23];
   case PACK3('I', 'O', 'F'):
      return &mano_mnemonics[24];
   default:
      return NULL;
   }
}

const ManoMnemonic* mano_mnemonic_find(const char* name) {
   if (!name)
      return NULL;
   return mano_mnemonic_find_n(name, strlen(name));
}

int mano_pseudo_id(const char* name, size_t len) {
   if (!name || len != 3)
      return -1;
   uint32_t k = PACK3(upper_ascii((unsigned char)name[0]),
                      upper_ascii((unsigned char)name[1]),
                      upper_ascii((unsigned char)name[2]));
   switch (k) {
   case PACK3('O', 'R', 'G'):
      return 0;
   case PACK3('D', 'E', 'C'):
      return 1;
   case PACK3('H', 'E', 'X'):
      return 2;
   case PACK3('E', 'N', 'D'):
      return 3;
   default:
      return -1;
   }
}

bool mano_decode(uint16_t word, ManoInstr* out) {
   if (!out)
      return false;

   int indirect = (word >> 15) & 1;
   int op = (word >> 12) & 7;

   if (op != 7) {
      out->kind = MANO_KIND_MEM;
      out->op = op;
      out->indirect = indirect;
      out->addr = word & MANO_ADDR_MASK;
      return true;
   }

   int idx;
   switch (word) {
   case 0x7800:
      idx = 7;
      break;
   case 0x7400:
      idx = 8;
      break;
   case 0x7200:
      idx = 9;
      break;
   case 0x7100:
      idx = 10;
      break;
   case 0x7080:
      idx = 11;
      break;
   case 0x7040:
      idx = 12;
      break;
   case 0x7020:
      idx = 13;
      break;
   case 0x7010:
      idx = 14;
      break;
   case 0x7008:
      idx = 15;
      break;
   case 0x7004:
      idx = 16;
      break;
   case 0x7002:
      idx = 17;
      break;
   case 0x7001:
      idx = 18;
      break;
   case 0xF800:
      idx = 19;
      break;
   case 0xF400:
      idx = 20;
      break;
   case 0xF200:
      idx = 21;
      break;
   case 0xF100:
      idx = 22;
      break;
   case 0xF080:
      idx = 23;
      break;
   case 0xF040:
      idx = 24;
      break;
   default:
      return false;
   }

   out->kind = indirect ? MANO_KIND_IO : MANO_KIND_REG;
   out->op = idx;
   out->indirect = 0;
   out->addr = 0;
   return true;
}
