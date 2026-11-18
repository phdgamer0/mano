#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void* xmalloc(size_t size) {
   if (size == 0)
      size = 1;
   void* p = malloc(size);
   if (!p)
      die("out of memory allocating %zu byte(s)", size);
   return p;
}

void* xcalloc(size_t count, size_t size) {
   if (count == 0 || size == 0)
      return xmalloc(1);
   if (count > SIZE_MAX / size)
      die("allocation size overflow");
   void* p = calloc(count, size);
   if (!p)
      die("out of memory allocating %zu byte(s)", count * size);
   return p;
}

void* xrealloc(void* ptr, size_t size) {
   if (size == 0)
      size = 1;
   void* p = realloc(ptr, size);
   if (!p)
      die("out of memory allocating %zu byte(s)", size);
   return p;
}

void* xreallocarray(void* ptr, size_t count, size_t size) {
   if (count == 0 || size == 0)
      return xrealloc(ptr, 1);
   if (count > SIZE_MAX / size)
      die("allocation size overflow");
   return xrealloc(ptr, count * size);
}

char* xstrdup(const char* s) {
   if (!s)
      return NULL;
   size_t len = strlen(s);
   char* p = xmalloc(len + 1);
   memcpy(p, s, len + 1);
   return p;
}

char* xstrndup(const char* s, size_t len) {
   if (!s)
      return NULL;
   size_t n = 0;
   while (n < len && s[n])
      n++;
   char* p = xmalloc(n + 1);
   memcpy(p, s, n);
   p[n] = '\0';
   return p;
}

MANO_NORETURN void die(const char* fmt, ...) {
   va_list ap;
   fputs("mano: ", stderr);
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
   exit(1);
}

size_t str_copy(char* dst, size_t cap, const char* src) {
   if (cap == 0)
      return 0;
   size_t i = 0;
   while (i + 1 < cap && src[i]) {
      dst[i] = src[i];
      i++;
   }
   dst[i] = '\0';
   return i;
}

void str_trim(char* s) {
   char* start = s;
   while (*start && isspace((unsigned char)*start))
      start++;
   size_t len = strlen(start);
   if (start != s)
      memmove(s, start, len + 1);
   while (len > 0 && isspace((unsigned char)s[len - 1]))
      s[--len] = '\0';
}

bool str_eq_ci(const char* a, const char* b) {
   return strcasecmp(a, b) == 0;
}
