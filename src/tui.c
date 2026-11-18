#define _POSIX_C_SOURCE 200809L

#include "tui.h"

#include "cpu.h"
#include "isa.h"
#include "util.h"

#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
   CP_TITLE = 1,
   CP_MNEM,
   CP_LABEL,
   CP_PSEUDO,
   CP_COMMENT,
   CP_NUM,
   CP_REG,
   CP_CHANGED,
   CP_DIM,
   CP_ERR,
   CP_OK,
   CP_CUR,
   CP_BRK,
   CP_PC,
   CP_AR,
};

#define REG_PANEL_W 30
#define MEM_VISIBLE_ROWS 4

typedef struct {
      const ManoProgram* prog;
      ManoCPU cpu;

      bool brk[MANO_MEM_WORDS];
      bool follow;
      bool running;
      bool ignore_break;
      size_t top;
      int mem_base;

      uint16_t p_ac, p_pc, p_ir, p_ar, p_dr, p_tr, p_inpr, p_outr;
      bool p_e, p_ien, p_fgi;
      bool have_prev;

      unsigned char inq[2048];
      size_t inq_tail;
      size_t inq_len;

      char out[4096];
      size_t out_total;

      char msg[256];
      bool msg_err;

      ManoArch asm_arch;
      int asm_opt;
      bool show_asm;

      char* asm_buf;
      char** asm_lines;
      size_t asm_nlines;
      int asm_addr_first[MANO_MEM_WORDS];
      int asm_addr_last[MANO_MEM_WORDS];
      size_t asm_top;
} Tui;

static void free_native_asm(Tui* t) {
   free(t->asm_lines);
   free(t->asm_buf);
   t->asm_lines = NULL;
   t->asm_buf = NULL;
   t->asm_nlines = 0;
}

static int parse_addr_comment(const char* s) {
   if (s[0] == '#' && s[1] == ' ')
      s += 2;
   else if (s[0] == '/' && s[1] == '/' && s[2] == ' ')
      s += 3;
   else if (s[0] == '@' && s[1] == ' ')
      s += 2;
   else
      return -1;

   if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) ||
       !isxdigit((unsigned char)s[2]) || s[3] != ':')
      return -1;

   int val = 0;
   for (int i = 0; i < 3; i++) {
      char c = s[i];
      int d = (c >= '0' && c <= '9')   ? (c - '0')
              : (c >= 'a' && c <= 'f') ? (c - 'a' + 10)
                                       : (c - 'A' + 10);
      val = (val << 4) | d;
   }
   return val;
}

static void rebuild_native_asm(Tui* t) {
   free_native_asm(t);
   for (int i = 0; i < MANO_MEM_WORDS; i++) {
      t->asm_addr_first[i] = -1;
      t->asm_addr_last[i] = -1;
   }

   /* tmpfile() instead of open_memstream(): the latter is missing on Windows. */
   FILE* tmp = tmpfile();
   if (!tmp)
      return;
   mano_backend_emit(tmp, t->prog, t->cpu.start_pc, t->asm_arch, t->asm_opt);
   long sz = ftell(tmp);
   if (sz < 0 || fseek(tmp, 0, SEEK_SET) != 0) {
      fclose(tmp);
      return;
   }
   char* buf = xmalloc((size_t)sz + 1);
   size_t got = fread(buf, 1, (size_t)sz, tmp);
   buf[got] = '\0';
   fclose(tmp);

   t->asm_buf = buf;

   /* Find _start: so we skip .data / .Ljtab boilerplate in the TUI pane. */
   char* p = strstr(buf, "_start:\n");
   if (!p)
      p = buf;

   size_t cap = 128;
   t->asm_lines = xmalloc(cap * sizeof *t->asm_lines);
   t->asm_nlines = 0;

   int cur_addr = -1;
   while (*p) {
      char* eol = strchr(p, '\n');
      if (eol)
         *eol = '\0';

      if (strncmp(p, ".section .note", 14) == 0) {
         if (!eol)
            break;
         p = eol + 1;
         continue;
      }

      if (t->asm_nlines == cap) {
         cap *= 2;
         t->asm_lines = xreallocarray(t->asm_lines, cap,
                                      sizeof *t->asm_lines);
      }
      size_t idx = t->asm_nlines++;
      t->asm_lines[idx] = p;

      int parsed = parse_addr_comment(p);
      if (parsed >= 0 && parsed < MANO_MEM_WORDS) {
         cur_addr = parsed;
         if (t->asm_addr_first[cur_addr] < 0)
            t->asm_addr_first[cur_addr] = (int)idx;
         t->asm_addr_last[cur_addr] = (int)idx;
      }
      else if (strcmp(p, ".Lbad:") == 0 || strncmp(p, ".section", 8) == 0) {
         cur_addr = -1;
      }
      else if (cur_addr >= 0 && *p != '\0') {
         t->asm_addr_last[cur_addr] = (int)idx;
      }

      if (!eol)
         break;
      p = eol + 1;
   }

   t->asm_top = 0;
}

static bool term_active;

static void term_cleanup(void) {
   if (term_active) {
      endwin();
      term_active = false;
   }
}

static void on_signal(int sig) {
   term_cleanup();
   signal(sig, SIG_DFL);
   raise(sig);
}

static void setmsg(Tui* t, bool err, const char* fmt, ...) MANO_PRINTF(3, 4);

static void setmsg(Tui* t, bool err, const char* fmt, ...) {
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(t->msg, sizeof t->msg, fmt, ap);
   va_end(ap);
   t->msg_err = err;
}

static int tui_read(void* ctx) {
   Tui* t = ctx;
   if (t->inq_len == 0)
      return -1;
   int ch = t->inq[t->inq_tail];
   t->inq_tail = (t->inq_tail + 1) % sizeof t->inq;
   t->inq_len--;
   return ch;
}

static void tui_write(void* ctx, int ch) {
   Tui* t = ctx;
   t->out[t->out_total % sizeof t->out] = (char)ch;
   t->out_total++;
}

static void snapshot(Tui* t) {
   t->p_ac = t->cpu.ac;
   t->p_pc = t->cpu.pc;
   t->p_ir = t->cpu.ir;
   t->p_ar = t->cpu.ar;
   t->p_dr = t->cpu.dr;
   t->p_tr = t->cpu.tr;
   t->p_inpr = t->cpu.inpr;
   t->p_outr = t->cpu.outr;
   t->p_e = t->cpu.e;
   t->p_ien = t->cpu.ien;
   t->p_fgi = t->cpu.fgi;
   t->have_prev = true;
}

static void do_step(Tui* t) {
   if (t->cpu.halted)
      return;
   snapshot(t);
   if (mano_cpu_step(&t->cpu) < 0)
      setmsg(t, true, "fault: invalid instruction %04X at %03X",
             (unsigned)t->cpu.fault_ir, (unsigned)t->cpu.fault_addr);
}

static bool tick(Tui* t) {
   if (t->cpu.halted) {
      t->running = false;
      return false;
   }
   if (t->brk[t->cpu.pc] && !t->ignore_break) {
      t->running = false;
      t->ignore_break = true;
      setmsg(t, false, "breakpoint at %03X", (unsigned)t->cpu.pc);
      return false;
   }
   t->ignore_break = false;
   do_step(t);
   return true;
}

static void init_colors(void) {
   if (!has_colors())
      return;
   start_color();
   short bg = COLOR_BLACK;
   if (use_default_colors() == OK)
      bg = -1;

   init_pair(CP_TITLE, COLOR_BLACK, COLOR_CYAN);
   init_pair(CP_MNEM, COLOR_GREEN, bg);
   init_pair(CP_LABEL, COLOR_YELLOW, bg);
   init_pair(CP_PSEUDO, COLOR_MAGENTA, bg);
   init_pair(CP_COMMENT, COLOR_CYAN, bg);
   init_pair(CP_NUM, COLOR_YELLOW, bg);
   init_pair(CP_REG, COLOR_WHITE, bg);
   init_pair(CP_CHANGED, COLOR_YELLOW, bg);
   init_pair(CP_DIM, COLOR_BLUE, bg);
   init_pair(CP_ERR, COLOR_RED, bg);
   init_pair(CP_OK, COLOR_GREEN, bg);
   init_pair(CP_CUR, COLOR_BLACK, COLOR_CYAN);
   init_pair(CP_BRK, COLOR_RED, bg);
   init_pair(CP_PC, COLOR_BLACK, COLOR_YELLOW);
   init_pair(CP_AR, COLOR_WHITE, COLOR_RED);
}

static int classify_token(const char* s, size_t n) {
   if (mano_mnemonic_find_n(s, n))
      return CP_MNEM;
   if (mano_pseudo_id(s, n) >= 0)
      return CP_PSEUDO;

   bool all_hex = (n > 0);
   for (size_t i = 0; i < n; i++)
      if (!isxdigit((unsigned char)s[i]))
         all_hex = false;
   if (all_hex)
      return CP_NUM;
   return CP_LABEL;
}

static void draw_tokens(int y, int x, int maxw, const ManoLine* ln) {
   const char* s = ln->text;
   size_t len = strlen(s);
   size_t i = 0;
   int col = 0;
   bool first = true;

   while (i < len && col < maxw) {
      while (i < len && isspace((unsigned char)s[i])) {
         if (col < maxw) {
            mvaddch(y, x + col, ' ');
            col++;
         }
         i++;
      }
      if (i >= len)
         break;

      size_t start = i;
      while (i < len && !isspace((unsigned char)s[i]) && s[i] != ',')
         i++;
      size_t tlen = i - start;
      bool comma = (i < len && s[i] == ',');
      int pair = (first && comma) ? CP_LABEL : classify_token(s + start, tlen);
      first = false;

      int n = (int)tlen;
      if (n > maxw - col)
         n = maxw - col;
      if (n > 0) {
         attron(COLOR_PAIR(pair));
         mvaddnstr(y, x + col, s + start, n);
         attroff(COLOR_PAIR(pair));
         col += n;
      }
      if (comma) {
         if (col < maxw) {
            attron(COLOR_PAIR(CP_LABEL));
            mvaddch(y, x + col, ',');
            attroff(COLOR_PAIR(CP_LABEL));
            col++;
         }
         i++;
      }
   }
}

static void draw_source_row(Tui* t, int y, size_t idx, int src_w, int pc_line) {
   const ManoProgram* p = t->prog;
   const ManoLine* ln = &p->lines[idx];
   int addr = p->line_addr[idx];
   bool cur = ((int)idx == pc_line);

   char gutter[16];
   if (addr >= 0)
      snprintf(gutter, sizeof gutter, "%c%03X ",
               t->brk[addr] ? 'B' : ' ', addr);
   else
      snprintf(gutter, sizeof gutter, "     ");

   if (cur) {
      attr_t attr = COLOR_PAIR(CP_CUR) | A_BOLD;
      attron(attr);
      mvhline(y, 0, ' ', src_w);
      mvaddnstr(y, 0, gutter, src_w);
      mvaddnstr(y, 5, ln->text, src_w - 5);
      if (ln->comment) {
         int cx = 5 + (int)strlen(ln->text) + 1;
         if (cx < src_w)
            mvaddnstr(y, cx, ln->comment, src_w - cx);
      }
      attroff(attr);
      return;
   }

   int gp = (addr >= 0 && t->brk[addr]) ? CP_BRK : CP_DIM;
   attron(COLOR_PAIR(gp));
   mvaddnstr(y, 0, gutter, 5);
   attroff(COLOR_PAIR(gp));

   draw_tokens(y, 5, src_w - 5, ln);

   if (ln->comment) {
      int cx = 5 + (int)strlen(ln->text) + 1;
      if (cx < src_w) {
         attron(COLOR_PAIR(CP_COMMENT));
         mvaddnstr(y, cx, ln->comment, src_w - cx);
         attroff(COLOR_PAIR(CP_COMMENT));
      }
   }
}

static void draw_reg_line(Tui* t, int y, int x, int w, const char* name,
                          const char* value, bool changed) {
   char line[64];
   snprintf(line, sizeof line, " %-3s %s", name, value);
   attr_t attr = COLOR_PAIR(changed ? CP_CHANGED : CP_REG);
   if (changed)
      attr |= A_BOLD;
   attron(attr);
   mvhline(y, x, ' ', w);
   mvaddnstr(y, x, line, w);
   attroff(attr);
   (void)t;
}

static void reg_hex(Tui* t, int* row, int x, int w, const char* name,
                    uint16_t value, uint16_t prev, int digits) {
   char buf[16];
   snprintf(buf, sizeof buf, "%0*X", digits, (unsigned)value);
   draw_reg_line(t, *row, x, w, name, buf,
                 t->have_prev && value != prev);
   (*row)++;
}

static void reg_bit(Tui* t, int* row, int x, int w, const char* name,
                    bool value, bool prev) {
   draw_reg_line(t, *row, x, w, name, value ? "1" : "0",
                 t->have_prev && value != prev);
   (*row)++;
}

static void draw_regs(Tui* t, int y0, int x, int h, int w) {
   attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
   mvhline(y0, x, ' ', w);
   mvaddnstr(y0, x, " REGISTERS", w);
   attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

   int row = y0 + 2;
   int limit = y0 + h;

   if (row < limit) reg_hex(t, &row, x, w, "PC", t->cpu.pc, t->p_pc, 3);
   if (row < limit) reg_hex(t, &row, x, w, "AR", t->cpu.ar, t->p_ar, 3);
   if (row < limit) reg_hex(t, &row, x, w, "IR", t->cpu.ir, t->p_ir, 4);
   if (row < limit) reg_hex(t, &row, x, w, "AC", t->cpu.ac, t->p_ac, 4);
   if (row < limit) reg_hex(t, &row, x, w, "DR", t->cpu.dr, t->p_dr, 4);
   if (row < limit) reg_hex(t, &row, x, w, "TR", t->cpu.tr, t->p_tr, 4);
   if (row < limit) reg_bit(t, &row, x, w, "E", t->cpu.e, t->p_e);
   if (row < limit) reg_bit(t, &row, x, w, "I", t->cpu.ien, t->p_ien);
   if (row < limit) reg_bit(t, &row, x, w, "FGI", t->cpu.fgi, t->p_fgi);
   if (row < limit) reg_hex(t, &row, x, w, "INP", t->cpu.inpr, t->p_inpr, 2);
   if (row < limit) reg_hex(t, &row, x, w, "OUT", t->cpu.outr, t->p_outr, 2);

   if (row + 1 < limit) {
      row++;
      char buf[48];
      snprintf(buf, sizeof buf, " steps %llu", t->cpu.steps);
      attron(COLOR_PAIR(CP_DIM));
      mvhline(row, x, ' ', w);
      mvaddnstr(row, x, buf, w);
      attroff(COLOR_PAIR(CP_DIM));
      row++;
   }
   if (row < limit) {
      const char* state = t->cpu.fault    ? "FAULT"
                          : t->cpu.halted ? "HALTED"
                          : t->running    ? "RUNNING"
                                          : "PAUSED";
      char buf[48];
      snprintf(buf, sizeof buf, " %s", state);
      int pair = t->cpu.fault || t->cpu.halted ? CP_ERR : CP_OK;
      attron(COLOR_PAIR(pair) | A_BOLD);
      mvhline(row, x, ' ', w);
      mvaddnstr(row, x, buf, w);
      attroff(COLOR_PAIR(pair) | A_BOLD);
   }
}

static void draw_memory(Tui* t, int y0, int w) {
   int max_base = MANO_MEM_WORDS - 8 * MEM_VISIBLE_ROWS;
   int base = t->mem_base & ~7;
   if (base < 0)
      base = 0;
   if (base > max_base)
      base = max_base;

   attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
   mvhline(y0, 0, ' ', w);
   char head[128];
   snprintf(head, sizeof head,
            " MEMORY %03X..%03X   [ ]=page  m=AR   AR=%03X  PC=%03X", base,
            base + 8 * MEM_VISIBLE_ROWS - 1, (unsigned)t->cpu.ar,
            (unsigned)t->cpu.pc);
   mvaddnstr(y0, 0, head, w);
   attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

   for (int r = 0; r < MEM_VISIBLE_ROWS; r++) {
      int rowaddr = base + r * 8;
      if (rowaddr >= MANO_MEM_WORDS)
         break;
      int y = y0 + 1 + r;

      char abuf[16];
      snprintf(abuf, sizeof abuf, "%03X:", rowaddr);
      attron(COLOR_PAIR(CP_DIM));
      mvaddnstr(y, 0, abuf, 4);
      attroff(COLOR_PAIR(CP_DIM));

      for (int j = 0; j < 8; j++) {
         int a = rowaddr + j;
         if (a >= MANO_MEM_WORDS)
            break;

         int pair;
         attr_t attr = A_NORMAL;
         if (a == (int)t->cpu.pc) {
            pair = CP_PC;
            attr |= A_BOLD;
         }
         else if (a == (int)t->cpu.ar) {
            pair = CP_AR;
            attr |= A_BOLD;
         }
         else if (t->prog->is_code[a]) {
            pair = CP_REG;
         }
         else if (t->cpu.mem[a]) {
            pair = CP_MNEM;
         }
         else {
            pair = CP_DIM;
         }

         char wbuf[16];
         snprintf(wbuf, sizeof wbuf, "%04X",
                  (unsigned)t->cpu.mem[a]);
         attr |= COLOR_PAIR(pair);
         attron(attr);
         mvaddnstr(y, 4 + j * 5, wbuf, 4);
         attroff(attr);
      }
   }
}

static void draw_output(Tui* t, int y, int w) {
   attron(COLOR_PAIR(CP_DIM));
   mvaddnstr(y, 0, "OUT:", 4);
   attroff(COLOR_PAIR(CP_DIM));

   size_t cap = sizeof t->out;
   size_t stored = t->out_total < cap ? t->out_total : cap;
   int maxw = w - 5;
   if (maxw < 0)
      maxw = 0;
   if ((size_t)maxw > stored)
      maxw = (int)stored;
   size_t start = t->out_total - (size_t)maxw;

   attron(COLOR_PAIR(CP_OK));
   for (int k = 0; k < maxw; k++) {
      unsigned char c = (unsigned char)t->out[(start + (size_t)k) % cap];
      char ch = (c >= 32 && c < 127) ? (char)c : (c == '\n' ? ' ' : '.');
      mvaddch(y, 4 + k, ch);
   }
   attroff(COLOR_PAIR(CP_OK));
}

static bool is_native_comment(const char* s) {
   return s[0] == '#' || s[0] == '@' || (s[0] == '/' && s[1] == '/');
}

static bool is_native_num_or_sym(const char* s, size_t n) {
   if (n == 0)
      return false;
   if (s[0] == '#' || s[0] == '.' || isdigit((unsigned char)s[0]))
      return true;
   if (s[0] == '-' && n > 1 && isdigit((unsigned char)s[1]))
      return true;
   return false;
}

static void draw_native_line(int y, int x, int w, const char* line, bool active) {
   if (w <= 0)
      return;

   if (active) {
      attr_t attr = COLOR_PAIR(CP_CUR) | A_BOLD;
      attron(attr);
      mvhline(y, x, ' ', w);
      mvaddnstr(y, x, line, w);
      attroff(attr);
      return;
   }

   size_t len = strlen(line);
   size_t i = 0;
   while (i < len && isspace((unsigned char)line[i]))
      i++;

   if (i >= len)
      return;

   if (is_native_comment(line + i)) {
      attron(COLOR_PAIR(CP_COMMENT));
      mvaddnstr(y, x, line, w);
      attroff(COLOR_PAIR(CP_COMMENT));
      return;
   }

   if (line[len - 1] == ':') {
      attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
      mvaddnstr(y, x, line, w);
      attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
      return;
   }

   if (line[i] == '.') {
      attron(COLOR_PAIR(CP_PSEUDO));
      mvaddnstr(y, x, line, w);
      attroff(COLOR_PAIR(CP_PSEUDO));
      return;
   }

   /* Instruction line: highlight opcode in green, immediates/labels in yellow,
    * registers and punctuation in white. */
   int col = 0;
   size_t pos = 0;
   bool first_tok = true;
   while (pos < len && col < w) {
      if (isspace((unsigned char)line[pos])) {
         mvaddch(y, x + col, ' ');
         col++;
         pos++;
         continue;
      }
      if (line[pos] == ',' || line[pos] == '[' || line[pos] == ']' ||
          line[pos] == '+' || line[pos] == '*') {
         attron(COLOR_PAIR(CP_DIM));
         mvaddch(y, x + col, line[pos]);
         attroff(COLOR_PAIR(CP_DIM));
         col++;
         pos++;
         continue;
      }
      size_t start = pos;
      while (pos < len && !isspace((unsigned char)line[pos]) &&
             line[pos] != ',' && line[pos] != '[' && line[pos] != ']' &&
             line[pos] != '+' && line[pos] != '*')
         pos++;
      size_t tlen = pos - start;
      int pair;
      if (first_tok) {
         pair = CP_MNEM;
         first_tok = false;
      }
      else if (is_native_num_or_sym(line + start, tlen)) {
         pair = CP_NUM;
      }
      else {
         pair = CP_REG;
      }
      int n = (int)tlen;
      if (n > w - col)
         n = w - col;
      if (n > 0) {
         attron(COLOR_PAIR(pair));
         mvaddnstr(y, x + col, line + start, n);
         attroff(COLOR_PAIR(pair));
         col += n;
      }
   }
}

static void draw_native_pane(Tui* t, int y0, int x, int h, int w) {
   if (w <= 0 || h <= 0)
      return;

   char head[64];
   snprintf(head, sizeof head, " NATIVE [%s -O%d]  t/o",
            mano_arch_name(t->asm_arch), t->asm_opt);
   attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
   mvhline(y0, x, ' ', w);
   mvaddnstr(y0, x, head, w);
   attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

   int body_rows = h - 1;
   if (body_rows <= 0 || t->asm_nlines == 0)
      return;

   /* Find active native block for current PC (walking back if fused at -O3). */
   int cur_first = -1;
   int cur_last = -1;
   int pc = (int)(t->cpu.pc & MANO_ADDR_MASK);
   if (t->asm_addr_first[pc] >= 0) {
      cur_first = t->asm_addr_first[pc];
      cur_last = t->asm_addr_last[pc];
   }
   else if (t->prog->is_code[pc]) {
      for (int back = 1; back <= 4 && pc - back >= 0; back++) {
         int a = pc - back;
         if (!t->prog->is_code[a])
            break;
         if (t->asm_addr_first[a] >= 0) {
            cur_first = t->asm_addr_first[a];
            cur_last = t->asm_addr_last[a];
            break;
         }
      }
   }

   if (t->follow && cur_first >= 0) {
      size_t f = (size_t)cur_first;
      size_t l = (size_t)cur_last;
      if (f < t->asm_top)
         t->asm_top = f;
      else if (l >= t->asm_top + (size_t)body_rows) {
         if (l - f + 1 >= (size_t)body_rows)
            t->asm_top = f;
         else
            t->asm_top = l - (size_t)body_rows + 1;
      }
   }
   if (t->asm_top >= t->asm_nlines)
      t->asm_top = t->asm_nlines - 1;

   for (int r = 0; r < body_rows; r++) {
      size_t idx = t->asm_top + (size_t)r;
      if (idx >= t->asm_nlines)
         break;
      bool active = (cur_first >= 0 && (int)idx >= cur_first &&
                     (int)idx <= cur_last);
      draw_native_line(y0 + 1 + r, x, w, t->asm_lines[idx], active);
   }
}

static void draw(Tui* t) {
   int h, w;
   getmaxyx(stdscr, h, w);

   erase();
   if (h < 18 || w < 64) {
      mvaddnstr(0, 0, "terminal too small: need at least 64x18", w - 1);
      refresh();
      return;
   }

   int src_rows = h - 10;
   int reg_w = t->show_asm ? ((w >= 110) ? 26 : 19) : REG_PANEL_W;
   int reg_x = w - reg_w;
   int left_avail = reg_x - 1;
   int src_w = left_avail;
   int asm_x = 0;
   int asm_w = 0;
   if (t->show_asm) {
      src_w = (left_avail - 1) * 45 / 100;
      if (src_w < 16)
         src_w = 16;
      asm_x = src_w + 1;
      asm_w = left_avail - src_w - 1;
   }

   const char* state = t->cpu.fault    ? "FAULT"
                       : t->cpu.halted ? "HALTED"
                       : t->running    ? "RUNNING"
                                       : "PAUSED";

   attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
   mvhline(0, 0, ' ', w);
   char title[256];
   snprintf(title, sizeof title, " MANO  %s   [%s]   step %llu   pc=%03X",
            t->prog->path, state, t->cpu.steps, (unsigned)t->cpu.pc);
   mvaddnstr(0, 0, title, w);
   attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

   int pc_line = -1;
   if (t->cpu.pc < MANO_MEM_WORDS) {
      int32_t li = t->prog->line_of[t->cpu.pc];
      if (li >= 0 && (size_t)li < t->prog->nlines)
         pc_line = (int)li;
   }

   if (t->follow && pc_line >= 0) {
      size_t p = (size_t)pc_line;
      if (p < t->top)
         t->top = p;
      else if (p >= t->top + (size_t)src_rows)
         t->top = p - (size_t)src_rows + 1;
   }
   if (t->prog->nlines == 0)
      t->top = 0;
   else if (t->top >= t->prog->nlines)
      t->top = t->prog->nlines - 1;

   for (int r = 0; r < src_rows; r++) {
      size_t idx = t->top + (size_t)r;
      if (idx >= t->prog->nlines)
         break;
      draw_source_row(t, 1 + r, idx, src_w, pc_line);
   }

   if (t->show_asm) {
      attron(COLOR_PAIR(CP_DIM));
      mvvline(1, src_w, ACS_VLINE, src_rows);
      attroff(COLOR_PAIR(CP_DIM));
      draw_native_pane(t, 1, asm_x, src_rows, asm_w);
   }

   draw_regs(t, 1, reg_x, src_rows, reg_w);

   attron(COLOR_PAIR(CP_DIM));
   mvhline(h - 9, 0, ACS_HLINE, w);
   attroff(COLOR_PAIR(CP_DIM));

   draw_memory(t, h - 8, w);
   draw_output(t, h - 3, w);

   attron(COLOR_PAIR(t->msg_err ? CP_ERR : CP_OK));
   mvhline(h - 2, 0, ' ', w);
   mvaddnstr(h - 2, 0, t->msg, w);
   attroff(COLOR_PAIR(t->msg_err ? CP_ERR : CP_OK));

   attron(COLOR_PAIR(CP_DIM));
   mvaddnstr(h - 1, 0,
             " s step  r run  R reset  b brk  a asm  t arch  o -O  i in  "
             "[/] mem  f follow  ? help  q quit",
             w);
   attroff(COLOR_PAIR(CP_DIM));

   refresh();
}

static void prompt_input(Tui* t) {
   int h, w;
   getmaxyx(stdscr, h, w);
   char buf[256];

   memset(buf, 0, sizeof buf);
   timeout(-1);
   echo();
   curs_set(1);
   attron(COLOR_PAIR(CP_TITLE));
   mvhline(h - 2, 0, ' ', w);
   mvaddnstr(h - 2, 0, "input> ", 7);
   attroff(COLOR_PAIR(CP_TITLE));
   move(h - 2, 7);

   int rc = getnstr(buf, (int)sizeof buf - 1);
   noecho();
   curs_set(0);

   if (rc != OK) {
      setmsg(t, false, "input cancelled");
      return;
   }

   size_t len = strlen(buf);
   size_t queued = 0;
   for (size_t i = 0; i < len; i++) {
      if (t->inq_len >= sizeof t->inq)
         break;
      size_t head = (t->inq_tail + t->inq_len) % sizeof t->inq;
      t->inq[head] = (unsigned char)buf[i];
      t->inq_len++;
      queued++;
   }
   if (t->inq_len < sizeof t->inq) {
      size_t head = (t->inq_tail + t->inq_len) % sizeof t->inq;
      t->inq[head] = '\n';
      t->inq_len++;
   }
   setmsg(t, false, "queued %zu input byte(s)", queued);
}

static void show_help(Tui* t) {
   int h, w;
   getmaxyx(stdscr, h, w);
   int hh = 20;
   int ww = 58;
   if (hh > h - 2)
      hh = h - 2;
   if (ww > w - 2)
      ww = w - 2;
   if (hh < 4 || ww < 10)
      return;

   WINDOW* win = newwin(hh, ww, (h - hh) / 2, (w - ww) / 2);
   if (!win)
      return;
   keypad(win, TRUE);
   box(win, 0, 0);

   wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
   mvwaddnstr(win, 0, 2, " MANO DEBUGGER ", ww - 4);
   wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

   static const char* const lines[] = {
       "",
       "  s          step one instruction",
       "  r / space  run or pause",
       "  R          reset machine (restores memory)",
       "  b          toggle breakpoint at PC",
       "  a          toggle side-by-side native ASM view",
       "  t / o      cycle native target / -O0..-O3",
       "  j / k      scroll native ASM pane",
       "  i          type input for INP",
       "  [ ]        move memory window by 8 words",
       "  m          move memory window to AR",
       "  arrows     scroll source   PgUp/PgDn page",
       "  f          follow PC        c clear output",
       "  ?          this help        q quit",
       "",
       "  colours: mnemonics green, pseudos magenta,",
       "  labels/numbers yellow, comments cyan,",
       "  PC yellow, AR red in the memory pane.",
   };
   int n = (int)(sizeof lines / sizeof lines[0]);
   for (int i = 0; i < n && i + 2 < hh; i++)
      mvwaddnstr(win, i + 2, 1, lines[i], ww - 2);

   wrefresh(win);
   t->running = false;
   wgetch(win);
   delwin(win);
   touchwin(stdscr);
}

static void scroll_source(Tui* t, long delta) {
   long top = (long)t->top + delta;
   if (top < 0)
      top = 0;
   t->top = (size_t)top;
   t->follow = false;
}

static void scroll_native(Tui* t, long delta) {
   long top = (long)t->asm_top + delta;
   if (top < 0)
      top = 0;
   t->asm_top = (size_t)top;
   t->follow = false;
}

static bool handle_key(Tui* t, int ch) {
   switch (ch) {
   case 'q':
   case 'Q':
      return false;

   case 'a':
   case 'A':
      t->show_asm = !t->show_asm;
      if (t->show_asm && !t->asm_buf)
         rebuild_native_asm(t);
      setmsg(t, false, "native asm %s (%s -O%d)",
             t->show_asm ? "on" : "off",
             mano_arch_name(t->asm_arch), t->asm_opt);
      break;

   case 't':
   case 'T':
      t->asm_arch = (ManoArch)(((int)t->asm_arch + 1) % 4);
      t->show_asm = true;
      rebuild_native_asm(t);
      t->follow = true;
      setmsg(t, false, "native target: %s -O%d",
             mano_arch_name(t->asm_arch), t->asm_opt);
      break;

   case 'o':
   case 'O':
      t->asm_opt = (t->asm_opt + 1) % 4;
      t->show_asm = true;
      rebuild_native_asm(t);
      t->follow = true;
      setmsg(t, false, "native target: %s -O%d",
             mano_arch_name(t->asm_arch), t->asm_opt);
      break;

   case 'j':
      if (t->show_asm)
         scroll_native(t, 1);
      break;
   case 'k':
      if (t->show_asm)
         scroll_native(t, -1);
      break;

   case 's':
   case 'S':
      t->running = false;
      t->ignore_break = true;
      do_step(t);
      t->follow = true;
      break;

   case 'r':
   case ' ':
      if (t->cpu.halted) {
         setmsg(t, false, "halted - press R to reset");
         break;
      }
      t->running = !t->running;
      if (t->running)
         setmsg(t, false, "running");
      else
         setmsg(t, false, "paused");
      break;

   case 'R':
      mano_cpu_reset(&t->cpu);
      t->running = false;
      t->ignore_break = false;
      t->have_prev = false;
      t->follow = true;
      t->out_total = 0;
      t->inq_len = 0;
      t->inq_tail = 0;
      setmsg(t, false, "machine reset");
      break;

   case 'b':
      t->brk[t->cpu.pc] = !t->brk[t->cpu.pc];
      setmsg(t, false, "breakpoint %s at %03X",
             t->brk[t->cpu.pc] ? "set" : "cleared",
             (unsigned)t->cpu.pc);
      break;

   case 'i':
      t->running = false;
      prompt_input(t);
      break;

   case 'c':
      t->out_total = 0;
      setmsg(t, false, "output cleared");
      break;

   case 'f':
      t->follow = true;
      break;

   case '?':
      show_help(t);
      break;

   case '[':
      t->mem_base -= 8;
      if (t->mem_base < 0)
         t->mem_base = 0;
      break;

   case ']':
      t->mem_base += 8;
      if (t->mem_base > MANO_MEM_WORDS - 8 * MEM_VISIBLE_ROWS)
         t->mem_base = MANO_MEM_WORDS - 8 * MEM_VISIBLE_ROWS;
      break;

   case 'm':
      t->mem_base = (int)(t->cpu.ar & ~7u);
      if (t->mem_base > MANO_MEM_WORDS - 8 * MEM_VISIBLE_ROWS)
         t->mem_base = MANO_MEM_WORDS - 8 * MEM_VISIBLE_ROWS;
      break;

   case KEY_UP:
      scroll_source(t, -1);
      break;
   case KEY_DOWN:
      scroll_source(t, 1);
      break;
   case KEY_PPAGE:
      scroll_source(t, -10);
      break;
   case KEY_NPAGE:
      scroll_source(t, 10);
      break;
   case KEY_HOME:
      t->top = 0;
      t->follow = false;
      break;
   case KEY_END:
      t->top = t->prog->nlines ? t->prog->nlines - 1 : 0;
      t->follow = false;
      break;
   default:
      break;
   }
   return true;
}

int mano_tui_run(const ManoProgram* prog, int start_pc, ManoArch arch,
                 int opt_level) {
   Tui* t = xcalloc(1, sizeof *t);
   t->prog = prog;
   t->follow = true;
   t->asm_arch = arch;
   t->asm_opt = (opt_level < 0) ? 0 : (opt_level > 3 ? 3 : opt_level);

   ManoIO io;
   memset(&io, 0, sizeof io);
   io.read = tui_read;
   io.write = tui_write;
   io.ctx = t;
   mano_cpu_init(&t->cpu, prog, start_pc, &io);
   t->mem_base = (int)(t->cpu.pc & ~7u);

   atexit(term_cleanup);
   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);
#ifdef SIGHUP
   signal(SIGHUP, on_signal);
#endif

   if (!initscr()) {
      fprintf(stderr, "mano: cannot initialise the terminal\n");
      free(t);
      return 1;
   }
   term_active = true;
   cbreak();
   noecho();
   keypad(stdscr, TRUE);
   curs_set(0);
   init_colors();

   setmsg(t, false, "ready - press ? for help");

   for (;;) {
      if (t->cpu.halted)
         t->running = false;

      draw(t);

      if (t->running) {
         timeout(16);
         for (int i = 0; i < 200 && t->running && !t->cpu.halted; i++) {
            if (!tick(t))
               break;
         }
         if (!t->running || t->cpu.halted)
            continue;
      }
      else {
         timeout(-1);
      }

      int ch = getch();
      if (ch == ERR || ch == KEY_RESIZE)
         continue;
      if (!handle_key(t, ch))
         break;
   }

   term_cleanup();
   free_native_asm(t);
   free(t);
   return 0;
}
