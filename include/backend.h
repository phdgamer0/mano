#ifndef MANO_BACKEND_H
#define MANO_BACKEND_H

#include "mano.h"

#include <stdbool.h>

typedef enum {
   MANO_ARCH_AMD64 = 0,
   MANO_ARCH_I386,
   MANO_ARCH_ARM64,
   MANO_ARCH_ARM32
} ManoArch;

bool mano_arch_parse(const char* name, ManoArch* out);
const char* mano_arch_name(ManoArch arch);
const char* mano_arch_compiler(ManoArch arch);

int mano_backend_write(const ManoProgram* prog, const char* path,
                       int start_pc, ManoArch arch, int opt_level);
void mano_backend_emit(FILE* f, const ManoProgram* prog, int start_pc,
                       ManoArch arch, int opt_level);

#endif
