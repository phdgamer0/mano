#ifndef MANO_ASM_H
#define MANO_ASM_H

#include "mano.h"

int mano_assemble_file(const char* path, ManoProgram* prog);
void mano_print_listing(const ManoProgram* prog, FILE* out);
void mano_print_symbols(const ManoProgram* prog, FILE* out);

#endif
