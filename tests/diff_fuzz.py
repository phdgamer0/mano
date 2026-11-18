#!/usr/bin/env python3
"""Differential test: random Mano programs must behave the same in the
simulator (`mano run`) and as native code at every -O level.

usage: diff_fuzz.py [MANO] [COUNT]; env TARGET=amd64|i386|arm64|arm32, SEED=n.
arm targets need the cross gcc plus qemu-user (qemu-aarch64 / qemu-arm)."""
import os, random, subprocess, sys, tempfile

MANO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./mano")
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
SEED = int(os.environ.get("SEED", "1"))
TARGET = os.environ.get("TARGET", "amd64")
CC, EXEC = {
    "amd64": (["cc", "-nostdlib", "-static", "-no-pie"], []),
    "i386": (["cc", "-m32", "-nostdlib", "-static", "-no-pie"], []),
    "arm64": (["aarch64-linux-gnu-gcc", "-nostdlib", "-static"], ["qemu-aarch64"]),
    "arm32": (["arm-linux-gnueabihf-gcc", "-nostdlib", "-static"], ["qemu-arm"]),
}[TARGET]

MEM = ["AND", "ADD", "LDA", "STA", "ISZ"]
REG = ["CLA", "CLE", "CMA", "CME", "CIR", "CIL", "INC", "SPA", "SNA", "SZA", "SZE"]


def gen(rng):
    ndata = rng.randint(2, 8)
    data = [f"D{i}" for i in range(ndata)]
    lines = ["        ORG 100"]
    for _ in range(rng.randint(5, 60)):
        r = rng.random()
        if r < 0.45:
            op = rng.choice(MEM)
            ind = " I P" + str(rng.randrange(2)) if rng.random() < 0.15 else " " + rng.choice(data)
            lines.append(f"        {op}{ind}")
        elif r < 0.85:
            lines.append("        " + rng.choice(REG))
        elif r < 0.95:
            lines.append("        OUT")
        else:
            lines.append("        INP")
    # Skips may jump over the HLT's predecessor, so pad before halting.
    lines += ["        OUT", "        CLA", "        HLT", "        HLT"]
    for d in data:
        lines.append(f"{d},      DEC {rng.randint(-32768, 65535)}")
    lines.append(f"P0,      HEX {0x200:X}")
    lines.append(f"P1,      HEX {0x201:X}")
    lines.append("        ORG 200")
    lines.append(f"        DEC {rng.randint(-100, 100)}")
    lines.append(f"        DEC {rng.randint(-100, 100)}")
    lines.append("        END")
    return "\n".join(lines) + "\n"


def run(cmd, stdin):
    p = subprocess.run(cmd, input=stdin, capture_output=True, timeout=10)
    return p.returncode, p.stdout


def main():
    rng = random.Random(SEED)
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "p.asm")
        for i in range(N):
            prog = gen(rng)
            stdin = bytes(rng.randrange(256) for _ in range(rng.randrange(4)))
            with open(src, "w") as f:
                f.write(prog)
            rc, want = run([MANO, "run", src], stdin)
            if rc != 0:
                print(f"[{i}] simulator rc={rc}\n{prog}")
                fails += 1
                continue
            for O in range(4):
                s, exe = os.path.join(td, "p.s"), os.path.join(td, "p")
                subprocess.run([MANO, "x86", "--target", TARGET, f"-O{O}", src, "-o", s], check=True,
                               capture_output=True)
                subprocess.run(CC + [s, "-o", exe], check=True)
                nrc, got = run(EXEC + [exe], stdin)
                if nrc != 0 or got != want:
                    print(f"[{i}] -O{O} mismatch rc={nrc} want={want!r} got={got!r}\n{prog}")
                    fails += 1
                    break
    print(f"{N - fails}/{N} programs OK (target {TARGET}, seed {SEED})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
