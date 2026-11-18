# mano

[![CI](https://github.com/phdgamer0/mano/actions/workflows/ci.yml/badge.svg)](https://github.com/phdgamer0/mano/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

An assembler, instruction-level simulator, full-screen debugger and **native code
compiler** for the *basic computer* from Morris Mano's *Computer System
Architecture* (3rd ed., ch. 5–6).

- **`asm`** – two-pass assembler with listing and symbol table.
- **`run`** – runs the program on the simulated 4096 × 16-bit machine, with
  optional per-instruction trace.
- **`debug`** – ncurses TUI debugger: step, run, breakpoints, live registers,
  memory pages, I/O, and a side-by-side view of the generated native assembly.
- **`x86`** – compiles the program to freestanding Linux assembly for
  **amd64, i386, arm64 or arm32**, with four optimization levels (`-O0`…`-O3`).

## Install

**Linux / macOS**

```sh
curl -fsSL https://github.com/phdgamer0/mano/releases/latest/download/install.sh | sh
```

Installs to `~/.local/bin` (override with `PREFIX=/usr/local`). The script
verifies the download against the release's `SHA256SUMS`.

**Windows (x64 and ARM64, PowerShell)**

```powershell
irm https://github.com/phdgamer0/mano/releases/latest/download/install.ps1 | iex
```

Installs to `%LOCALAPPDATA%\Programs\mano` and adds it to your user `PATH`.

**Debian / Ubuntu** – download the `.deb` for your architecture from the
[releases page](https://github.com/phdgamer0/mano/releases) and run
`sudo apt install ./mano_*.deb`.

**Arch Linux** – a `PKGBUILD` is in [`packaging/aur`](packaging/aur):
`cd packaging/aur && makepkg -si`.

Prebuilt binaries: Linux (x86_64, aarch64; static), macOS (arm64, x86_64),
Windows (x86_64, arm64; static).

## Build from source

Requirements: a C11 compiler (GCC or Clang), CMake ≥ 3.16, and ncurses
(`libncurses-dev` on Debian/Ubuntu, `ncurses` on Arch, built in on macOS,
`mingw-w64-*-ncurses` under MSYS2 on Windows).

```sh
make              # release build, binary at ./mano
make asan         # AddressSanitizer + UBSan build
make install      # installs to ~/.local/bin (PREFIX=... to change)
```

or with CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake options: `-DMANO_ASAN=ON` (sanitizers), `-DMANO_STATIC=ON` (fully
static executable). MSVC is not supported; on Windows build inside an
[MSYS2](https://www.msys2.org/) UCRT64 or CLANGARM64 shell.

## Usage

```
mano <command> [options] <file.asm>

  asm     assemble and print the listing plus symbol table
  run     assemble and execute on the simulated machine (default)
  debug   assemble and open the full-screen debugger
  x86     compile to freestanding Linux assembly (GNU syntax)

  --trace          run: print every instruction
  --max N          run: step limit (default 1000000)
  --pc ADDR        start PC in hex (default: first instruction)
  --target ARCH    x86: amd64 | i386 | arm64 | arm32 (default: amd64)
  -O0..-O3         x86: optimization level (default: 0)
  -o FILE          x86: output file (default: <input>[.<arch>].s)
  -V, --version    show the version
```

### Example

```asm
        ORG 100
        LDA A
        ADD B
        STA C
        OUT         / prints '*' (42)
        HLT
A,      DEC 7
B,      DEC 35
C,      DEC 0
        END
```

```console
$ mano asm examples/sum.asm
ADDR  WORD  SOURCE
100   2105  LDA A
101   1106  ADD B
102   3107  STA C
103   F400  OUT
104   7001  HLT
...
$ mano run examples/sum.asm
*
[halted after 5 instructions]
AC=002A E=0 I=0 PC=105 ...

$ mano x86 -O3 examples/sum.asm          # writes examples/sum.s
$ cc -nostdlib -static -no-pie examples/sum.s -o sum && ./sum
*
```

Cross targets print the matching build command, e.g.
`mano x86 --target arm64 file.asm` suggests
`aarch64-linux-gnu-gcc -nostdlib -static file.arm64.s -o file.arm64`.

### Source syntax

| Syntax            | Meaning                                  |
| ----------------- | ---------------------------------------- |
| `ORG 100`         | set the location counter (hex)           |
| `LABEL, DEC -12`  | label with a decimal word                |
| `LABEL, HEX 0A3F` | label with a hex word                    |
| `LDA I X`         | indirect addressing                      |
| `/` or `;`        | start a comment                          |
| `END`             | end of source                            |

All 25 Mano instructions are supported: `AND ADD LDA STA BUN BSA ISZ`,
`CLA CLE CMA CME CIR CIL INC SPA SNA SZA SZE HLT`, `INP OUT SKI SKO ION IOF`.
`INP` reads a byte from stdin (0 at end of input); `OUT` writes the low byte of AC.

### Debugger keys

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| `s` | step | `r` / space | run / pause |
| `R` | reset | `b` | breakpoint at PC |
| `a` | toggle native asm split | `t` / `o` | cycle target / opt level |
| `i` | queue input | `[` `]` | memory page |
| `m` | memory at AR | `f` | follow PC |
| `c` | clear output | `?` / `q` | help / quit |

## Testing

```sh
bash tests/smoke.sh ./mano                 # every platform
python3 tests/diff_fuzz.py ./mano 500      # Linux: simulator vs native code
TARGET=arm64 python3 tests/diff_fuzz.py ./mano   # needs cross gcc + qemu-user
```

`diff_fuzz.py` generates random programs and checks that the simulator and the
compiled native binary produce identical output at every optimization level.
CI runs it for all four targets under AddressSanitizer and UBSan, and builds
and smoke-tests on Linux, macOS and Windows for both x86_64 and ARM64.

## Contributing

Issues and pull requests are welcome. Format C sources with the bundled
`.clang-format` (`clang-format -i src/*.[ch] include/*.h`) and make sure
`tests/smoke.sh` passes.

## Security

See [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE) © 2026 Yazdan Samari
