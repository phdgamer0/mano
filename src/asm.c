#define _POSIX_C_SOURCE 200809L

#include "asm.h"

#include "isa.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
      uint32_t off;
      uint32_t len;
} Span;

enum {
   PSEUDO_NONE = -1,
   PSEUDO_ORG = 0,
   PSEUDO_DEC = 1,
   PSEUDO_HEX = 2,
   PSEUDO_END = 3,
};

typedef struct {
      Span label;
      Span mnem;
      Span operand;
      const ManoMnemonic* mn;
      int value;
      int line;
      int addr;
      int8_t pseudo;
      uint8_t flags;
} Stmt;

enum {
   ST_LABEL = 1 << 0,
   ST_MNEM = 1 << 1,
   ST_OPERAND = 1 << 2,
   ST_INDIRECT = 1 << 3,
   ST_VALUE = 1 << 4,
};

typedef struct {
      ManoProgram* prog;
      const char* path;
      Stmt* stmts;
      size_t nstmts;
      int errors;
} Asm;

static inline bool is_ascii_space(unsigned char c) {
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
          c == '\f';
}

static inline bool is_ascii_alpha(unsigned char c) {
   unsigned char l = c | 0x20u;
   return l >= 'a' && l <= 'z';
}

static inline bool is_ascii_digit(unsigned char c) {
   return c >= '0' && c <= '9';
}

static inline int hex_digit(unsigned char c) {
   if (c >= '0' && c <= '9')
      return (int)(c - '0');
   unsigned char l = c | 0x20u;
   if (l >= 'a' && l <= 'f')
      return (int)(l - 'a' + 10);
   return -1;
}

static void aerr(Asm* a, int lineno, const char* fmt, ...) MANO_PRINTF(3, 4);

static void aerr(Asm* a, int lineno, const char* fmt, ...) {
   va_list ap;
   fprintf(stderr, "%s:%d: error: ", a->path, lineno);
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
   a->errors++;
}

static void span_to_buf(const char* src, Span sp, char* dst, size_t cap) {
   if (cap == 0)
      return;
   size_t n = sp.len;
   if (n > cap - 1)
      n = cap - 1;
   memcpy(dst, src + sp.off, n);
   dst[n] = '\0';
}

static bool span_copy(const char* src, Span sp, char* dst, size_t cap) {
   if (cap == 0 || (size_t)sp.len + 1 > cap)
      return false;
   memcpy(dst, src + sp.off, sp.len);
   dst[sp.len] = '\0';
   return true;
}

static bool scan_token(const char* s, size_t* pos, Span* out, bool* comma) {
   size_t i = *pos;
   while (s[i] && is_ascii_space((unsigned char)s[i]))
      i++;
   if (!s[i]) {
      *pos = i;
      return false;
   }
   size_t start = i;
   while (s[i] && !is_ascii_space((unsigned char)s[i]) && s[i] != ',')
      i++;
   size_t end = i;
   bool had_comma = false;
   if (s[i]) {
      had_comma = (s[i] == ',');
      i++;
   }
   out->off = (uint32_t)start;
   out->len = (uint32_t)(end - start);
   *pos = i;
   if (comma)
      *comma = had_comma;
   return true;
}

static bool span_parse_hex(const char* src, Span sp, int* out) {
   if (sp.len == 0 || sp.len >= 32)
      return false;
   const char* p = src + sp.off;
   const char* end = p + sp.len;

   bool neg = false;
   if (*p == '+' || *p == '-') {
      neg = (*p == '-');
      p++;
   }
   if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
      p += 2;
   if (p >= end)
      return false;

   uint32_t v = 0;
   for (; p < end; p++) {
      int d = hex_digit((unsigned char)*p);
      if (d < 0)
         return false;
      v = (v << 4) | (uint32_t)d;
      if (v > 0xFFFFu)
         return false;
   }
   *out = neg ? -(int)v : (int)v;
   return true;
}

static bool span_parse_dec(const char* src, Span sp, int* out) {
   if (sp.len == 0 || sp.len >= 32)
      return false;
   const char* p = src + sp.off;
   const char* end = p + sp.len;

   bool neg = false;
   if (*p == '+' || *p == '-') {
      neg = (*p == '-');
      p++;
   }
   if (p >= end)
      return false;

   uint32_t v = 0;
   for (; p < end; p++) {
      unsigned char c = (unsigned char)*p;
      if (!is_ascii_digit(c))
         return false;
      v = v * 10u + (uint32_t)(c - '0');
      if (v > 65535u)
         return false;
   }
   long sv = neg ? -(long)v : (long)v;
   if (sv < -32768 || sv > 65535)
      return false;
   *out = (int)sv;
   return true;
}

static bool valid_label(const char* s) {
   unsigned char c = (unsigned char)s[0];
   if (!(is_ascii_alpha(c) || c == '_'))
      return false;
   for (size_t i = 1; s[i]; i++) {
      c = (unsigned char)s[i];
      if (!(is_ascii_alpha(c) || is_ascii_digit(c) || c == '_'))
         return false;
   }
   return true;
}

static char* read_whole_file(FILE* f, size_t* out_len) {
   struct stat st;
   if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
      size_t sz = (size_t)st.st_size;
      char* buf = xmalloc(sz + 1);
      size_t got = fread(buf, 1, sz, f);
      buf[got] = '\0';
      *out_len = got;
      return buf;
   }

   size_t cap = 4096;
   size_t len = 0;
   char* buf = xmalloc(cap + 1);
   for (;;) {
      if (len == cap) {
         cap *= 2;
         buf = xrealloc(buf, cap + 1);
      }
      size_t got = fread(buf + len, 1, cap - len, f);
      len += got;
      if (got == 0)
         break;
   }
   buf[len] = '\0';
   *out_len = len;
   return buf;
}

static int load_lines(Asm* a) {
   FILE* f = fopen(a->path, "r");
   if (!f) {
      fprintf(stderr, "mano: cannot open '%s': %s\n", a->path,
              strerror(errno));
      return -1;
   }

   size_t raw_len = 0;
   char* raw = read_whole_file(f, &raw_len);
   bool failed = ferror(f) != 0;
   fclose(f);
   if (failed) {
      free(raw);
      fprintf(stderr, "mano: error reading '%s'\n", a->path);
      return -1;
   }

   size_t n_nl = 1;
   for (size_t i = 0; i < raw_len; i++) {
      if (raw[i] == '\n')
         n_nl++;
   }

   ManoProgram* p = a->prog;
   /* Single packed arena for all trimmed line texts and comments. */
   size_t arena_cap = raw_len + n_nl * 2 + 4;
   p->src_buf = xmalloc(arena_cap);
   p->lines = xmalloc(n_nl * sizeof *p->lines);
   p->lines_cap = n_nl;
   p->nlines = 0;

   char* dst = p->src_buf;
   int lineno = 0;
   size_t pos = 0;

   while (pos < raw_len) {
      lineno++;
      size_t lstart = pos;
      const char* nl = memchr(raw + pos, '\n', raw_len - pos);
      size_t lend = nl ? (size_t)(nl - raw) : raw_len;
      pos = nl ? (lend + 1) : raw_len;

      while (lend > lstart &&
             (raw[lend - 1] == '\r' || raw[lend - 1] == '\n'))
         lend--;

      size_t cat = lend;
      for (size_t i = lstart; i < lend; i++) {
         if (raw[i] == ';' || raw[i] == '/') {
            cat = i;
            break;
         }
      }

      size_t t0 = lstart, t1 = cat;
      while (t0 < t1 && is_ascii_space((unsigned char)raw[t0]))
         t0++;
      while (t1 > t0 && is_ascii_space((unsigned char)raw[t1 - 1]))
         t1--;

      size_t c0 = cat, c1 = lend;
      while (c0 < c1 && is_ascii_space((unsigned char)raw[c0]))
         c0++;
      while (c1 > c0 && is_ascii_space((unsigned char)raw[c1 - 1]))
         c1--;

      if (t0 == t1 && c0 == c1)
         continue;

      if (p->nlines >= (size_t)INT32_MAX)
         die("source file has too many lines");

      char* text = dst;
      size_t tlen = t1 - t0;
      if (tlen) {
         memcpy(dst, raw + t0, tlen);
         dst += tlen;
      }
      *dst++ = '\0';

      char* comment = NULL;
      size_t clen = c1 - c0;
      if (clen) {
         comment = dst;
         memcpy(dst, raw + c0, clen);
         dst += clen;
         *dst++ = '\0';
      }

      ManoLine* ln = &p->lines[p->nlines++];
      ln->text = text;
      ln->comment = comment;
      ln->lineno = lineno;
   }

   free(raw);
   return 0;
}

static void parse_line(Asm* a, Stmt* st) {
   const char* s = a->prog->lines[st->line].text;
   int lineno = a->prog->lines[st->line].lineno;
   size_t pos = 0;
   Span t;
   bool comma = false;
   char tok[40];

   if (!scan_token(s, &pos, &t, &comma))
      return;

   int pid = -1;
   const ManoMnemonic* m = NULL;

   if (comma) {
      st->label = t;
      st->flags |= ST_LABEL;
      if (!scan_token(s, &pos, &t, &comma))
         return;
      pid = mano_pseudo_id(s + t.off, t.len);
      if (pid < 0)
         m = mano_mnemonic_find_n(s + t.off, t.len);
   }
   else {
      pid = mano_pseudo_id(s + t.off, t.len);
      if (pid < 0)
         m = mano_mnemonic_find_n(s + t.off, t.len);
      if (pid < 0 && !m) {
         span_to_buf(s, t, tok, sizeof tok);
         aerr(a, lineno,
              "unknown instruction '%s' (labels need a trailing comma)",
              tok);
         return;
      }
   }

   st->mnem = t;
   st->flags |= ST_MNEM;

   if (pid >= 0) {
      st->pseudo = (int8_t)pid;
      if (pid == PSEUDO_ORG || pid == PSEUDO_DEC || pid == PSEUDO_HEX) {
         if (!scan_token(s, &pos, &t, &comma)) {
            span_to_buf(s, st->mnem, tok, sizeof tok);
            aerr(a, lineno, "'%s' expects a value", tok);
            st->flags &= (uint8_t)~ST_MNEM;
            st->pseudo = PSEUDO_NONE;
            return;
         }
         st->operand = t;
         st->flags |= ST_OPERAND;
         if (scan_token(s, &pos, &t, &comma))
            aerr(a, lineno, "unexpected extra operand");
      }
      else if (scan_token(s, &pos, &t, &comma)) {
         aerr(a, lineno, "END takes no operand");
      }
      return;
   }

   if (!m) {
      st->flags &= (uint8_t)~ST_MNEM;
      return;
   }
   st->mn = m;

   if (m->kind == MANO_KIND_MEM) {
      if (!scan_token(s, &pos, &t, &comma)) {
         aerr(a, lineno, "%s expects an address", m->name);
         st->mn = NULL;
         return;
      }
      if (t.len == 1 && (s[t.off] == 'I' || s[t.off] == 'i')) {
         st->flags |= ST_INDIRECT;
         if (!scan_token(s, &pos, &t, &comma)) {
            aerr(a, lineno, "%s I expects an address", m->name);
            st->mn = NULL;
            return;
         }
      }
      st->operand = t;
      st->flags |= ST_OPERAND;
      if (scan_token(s, &pos, &t, &comma))
         aerr(a, lineno, "unexpected extra operand");
   }
   else if (scan_token(s, &pos, &t, &comma)) {
      aerr(a, lineno, "%s takes no operand", m->name);
   }
}

static void parse_all(Asm* a) {
   size_t n = a->prog->nlines;
   a->stmts = xcalloc(n ? n : 1, sizeof *a->stmts);
   a->nstmts = n;
   for (size_t i = 0; i < n; i++) {
      Stmt* st = &a->stmts[i];
      st->pseudo = PSEUDO_NONE;
      st->addr = -1;
      st->line = (int)i;
      parse_line(a, st);
   }
}

static void pass1(Asm* a) {
   ManoProgram* p = a->prog;
   int lc = 0;

   for (size_t i = 0; i < a->nstmts; i++) {
      Stmt* st = &a->stmts[i];
      const char* s = p->lines[st->line].text;
      int lineno = p->lines[st->line].lineno;

      if (st->flags & ST_LABEL) {
         char name[MANO_MAX_NAME + 1];
         if (!span_copy(s, st->label, name, sizeof name)) {
            aerr(a, lineno, "label longer than %d characters",
                 MANO_MAX_NAME);
         }
         else if (!valid_label(name)) {
            aerr(a, lineno, "invalid label name '%s'", name);
         }
         else if (mano_mnemonic_find_n(name, st->label.len) ||
                  mano_pseudo_id(name, st->label.len) >= 0) {
            aerr(a, lineno, "label '%s' conflicts with an instruction",
                 name);
         }
         else {
            int rc = mano_symtab_add(&p->syms, name, lc);
            if (rc == -1)
               aerr(a, lineno, "duplicate label '%s'", name);
            else if (rc < 0)
               die("out of memory building the symbol table");
         }
      }

      if (!(st->flags & ST_MNEM))
         continue;
      if (st->pseudo == PSEUDO_END)
         break;

      if (st->pseudo == PSEUDO_ORG) {
         int v;
         if (!(st->flags & ST_OPERAND))
            continue;
         if (!span_parse_hex(s, st->operand, &v)) {
            char tok[40];
            span_to_buf(s, st->operand, tok, sizeof tok);
            aerr(a, lineno, "invalid hex address '%s'", tok);
            continue;
         }
         if (v < 0 || v >= MANO_MEM_WORDS) {
            aerr(a, lineno, "ORG address %03X outside 000...FFF",
                 (unsigned)v);
            continue;
         }
         lc = v;
         continue;
      }

      if (st->pseudo == PSEUDO_DEC || st->pseudo == PSEUDO_HEX) {
         int v = 0;
         if (!(st->flags & ST_OPERAND))
            continue;
         bool ok = (st->pseudo == PSEUDO_DEC)
                       ? span_parse_dec(s, st->operand, &v)
                       : span_parse_hex(s, st->operand, &v);
         if (!ok) {
            char tok[40];
            span_to_buf(s, st->operand, tok, sizeof tok);
            aerr(a, lineno, "invalid %s value '%s'",
                 st->pseudo == PSEUDO_DEC ? "decimal" : "hex", tok);
            continue;
         }
         st->value = v;
         st->flags |= ST_VALUE;
      }

      if (lc >= MANO_MEM_WORDS) {
         aerr(a, lineno, "program exceeds %d words of memory",
              MANO_MEM_WORDS);
         continue;
      }
      st->addr = lc++;
   }
}

static bool resolve_operand(Asm* a, const Stmt* st, int* out) {
   const char* s = a->prog->lines[st->line].text;
   int lineno = a->prog->lines[st->line].lineno;
   Span sp = st->operand;
   char tok[40];

   if (sp.len == 0) {
      aerr(a, lineno, "missing operand");
      return false;
   }

   if (is_ascii_digit((unsigned char)s[sp.off])) {
      int v;
      if (!span_parse_hex(s, sp, &v)) {
         span_to_buf(s, sp, tok, sizeof tok);
         aerr(a, lineno, "invalid address '%s'", tok);
         return false;
      }
      if (v < 0 || v >= MANO_MEM_WORDS) {
         aerr(a, lineno, "address %03X outside 000...FFF", (unsigned)v);
         return false;
      }
      *out = v;
      return true;
   }

   char name[MANO_MAX_NAME + 1];
   if (!span_copy(s, sp, name, sizeof name)) {
      aerr(a, lineno, "symbol name longer than %d characters", MANO_MAX_NAME);
      return false;
   }
   int addr = mano_symtab_find(&a->prog->syms, name);
   if (addr < 0) {
      aerr(a, lineno, "undefined symbol '%s'", name);
      return false;
   }
   *out = addr;
   return true;
}

static void pass2(Asm* a) {
   ManoProgram* p = a->prog;

   for (size_t i = 0; i < a->nstmts; i++) {
      Stmt* st = &a->stmts[i];
      if (st->addr < 0)
         continue;

      if (st->pseudo == PSEUDO_DEC || st->pseudo == PSEUDO_HEX) {
         if (!(st->flags & ST_VALUE))
            continue;
         p->mem[st->addr] = (uint16_t)(st->value & MANO_WORD_MASK);
         p->is_code[st->addr] = 0;
      }
      else if (st->mn) {
         uint16_t word;
         if (st->mn->kind == MANO_KIND_MEM) {
            int addr;
            if (!resolve_operand(a, st, &addr))
               continue;
            word = (uint16_t)((st->mn->code << 12) |
                              ((st->flags & ST_INDIRECT) ? 0x8000 : 0) |
                              (addr & MANO_ADDR_MASK));
         }
         else {
            word = (uint16_t)st->mn->code;
         }
         p->mem[st->addr] = word;
         p->is_code[st->addr] = 1;
      }
      else {
         continue;
      }

      p->line_of[st->addr] = (int32_t)st->line;
      p->line_addr[st->line] = st->addr;
   }
}

int mano_assemble_file(const char* path, ManoProgram* prog) {
   Asm a;
   memset(&a, 0, sizeof a);
   a.prog = prog;
   a.path = path;
   prog->path = xstrdup(path);

   if (load_lines(&a) < 0)
      return -1;

   size_t la_count = prog->nlines ? prog->nlines : 1;
   prog->line_addr = xmalloc(la_count * sizeof *prog->line_addr);
   memset(prog->line_addr, 0xFF, la_count * sizeof *prog->line_addr);

   parse_all(&a);
   if (a.errors == 0) {
      pass1(&a);
      if (a.errors == 0)
         pass2(&a);
   }

   free(a.stmts);
   a.stmts = NULL;

   if (a.errors)
      return -1;

   prog->first_code = -1;
   prog->last_word = -1;
   for (int i = 0; i < MANO_MEM_WORDS; i++)
      if (prog->is_code[i]) {
         prog->first_code = i;
         break;
      }
   for (int i = MANO_MEM_WORDS - 1; i >= 0; i--)
      if (prog->mem[i]) {
         prog->last_word = i;
         break;
      }

   if (prog->first_code < 0) {
      fprintf(stderr, "%s: error: program contains no instructions\n", path);
      return -1;
   }
   return 0;
}

void mano_print_listing(const ManoProgram* prog, FILE* out) {
   fprintf(out, "ADDR  WORD  SOURCE\n");
   for (int addr = 0; addr < MANO_MEM_WORDS; addr++) {
      int32_t li = prog->line_of[addr];
      if (li < 0 || (size_t)li >= prog->nlines)
         continue;
      fprintf(out, "%03X   %04X  %s\n", addr, (unsigned)prog->mem[addr],
              prog->lines[li].text);
   }
}

static int cmp_symbol(const void* va, const void* vb) {
   const ManoSymbol* a = *(const ManoSymbol* const*)va;
   const ManoSymbol* b = *(const ManoSymbol* const*)vb;
   if (a->addr != b->addr)
      return (a->addr > b->addr) - (a->addr < b->addr);
   return strcmp(a->name, b->name);
}

void mano_print_symbols(const ManoProgram* prog, FILE* out) {
   if (prog->syms.count == 0)
      return;

   const ManoSymbol** sorted =
       xmalloc(prog->syms.count * sizeof *sorted);
   for (size_t i = 0; i < prog->syms.count; i++)
      sorted[i] = &prog->syms.items[i];
   qsort(sorted, prog->syms.count, sizeof *sorted, cmp_symbol);

   fprintf(out, "\nSYMBOL  ADDR\n");
   for (size_t i = 0; i < prog->syms.count; i++)
      fprintf(out, "%-7s %03X\n", sorted[i]->name, sorted[i]->addr);
   free(sorted);
}
