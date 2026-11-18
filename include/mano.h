#ifndef MANO_MANO_H
#define MANO_MANO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MANO_MEM_WORDS 4096
#define MANO_ADDR_MASK 0x0FFFu
#define MANO_WORD_MASK 0xFFFFu
#define MANO_MAX_NAME 32

typedef struct {
      char* text;
      char* comment;
      int lineno;
} ManoLine;

typedef struct {
      char name[MANO_MAX_NAME + 1];
      int addr;
} ManoSymbol;

typedef struct {
      ManoSymbol* items;
      size_t count;
      size_t cap;
      int32_t* buckets;
      size_t nbuckets;
} ManoSymTab;

typedef struct {
      uint16_t mem[MANO_MEM_WORDS];
      uint8_t is_code[MANO_MEM_WORDS];
      int32_t line_of[MANO_MEM_WORDS];
      int32_t* line_addr;
      ManoLine* lines;
      size_t nlines;
      size_t lines_cap;
      char* src_buf;
      char* path;
      ManoSymTab syms;
      int first_code;
      int last_word;
} ManoProgram;

void mano_program_init(ManoProgram* prog);
void mano_program_free(ManoProgram* prog);

void mano_symtab_free(ManoSymTab* tab);
int mano_symtab_find(const ManoSymTab* tab, const char* name);
int mano_symtab_add(ManoSymTab* tab, const char* name, int addr);

#endif
