# Security Policy

## Supported versions

| Version | Supported |
| ------- | --------- |
| 1.0.x   | yes       |
| < 1.0   | no        |

## Reporting a vulnerability

Please **do not open a public issue** for security problems.

Report privately through GitHub:
[Security → Report a vulnerability](https://github.com/phdgamer0/mano/security/advisories/new).

Include the mano version (`mano --version`), your OS and CPU architecture, and
a minimal `.asm` file or command line that reproduces the problem. You can
expect an acknowledgement within 7 days and a fix or mitigation plan within
30 days for confirmed issues. Reporters are credited in the release notes
unless they prefer otherwise.

## Scope

mano reads untrusted assembly source files, so the following are in scope:

- memory-safety bugs (overflows, use-after-free, leaks that can be triggered
  repeatedly) in the assembler, simulator, code generators or debugger;
- crashes or hangs caused by a crafted source file (other than a program that
  legitimately loops forever; use `--max` to bound `run`);
- generated native code that reads or writes outside the program's own
  4096-word memory image.

Out of scope: the behaviour of a Mano program itself, and anything requiring
an attacker who can already run arbitrary commands as the user.

## Hardening in place

- Every release is built and tested in CI with AddressSanitizer,
  UndefinedBehaviorSanitizer and LeakSanitizer.
- `tests/diff_fuzz.py` cross-checks the simulator against native code on all
  four targets; the assembler is mutation-fuzzed during development.
- Release downloads are published with `SHA256SUMS`, and the install scripts
  verify them before installing.
