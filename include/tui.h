#ifndef MANO_TUI_H
#define MANO_TUI_H

#include "backend.h"
#include "mano.h"

int mano_tui_run(const ManoProgram* prog, int start_pc, ManoArch arch,
                 int opt_level);

#endif
