#ifndef MANO_UTIL_H
#define MANO_UTIL_H

#include <stdbool.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define MANO_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define MANO_NORETURN __attribute__((noreturn))
#else
#define MANO_PRINTF(fmt_idx, arg_idx)
#define MANO_NORETURN
#endif

void* xmalloc(size_t size);
void* xcalloc(size_t count, size_t size);
void* xrealloc(void* ptr, size_t size);
void* xreallocarray(void* ptr, size_t count, size_t size);
char* xstrdup(const char* s);
char* xstrndup(const char* s, size_t len);

MANO_NORETURN void die(const char* fmt, ...) MANO_PRINTF(1, 2);

size_t str_copy(char* dst, size_t cap, const char* src);
void str_trim(char* s);
bool str_eq_ci(const char* a, const char* b);

#endif
