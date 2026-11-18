#define _POSIX_C_SOURCE 200809L

#include "mano.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void mano_program_init(ManoProgram* prog) {
   memset(prog, 0, sizeof *prog);
   memset(prog->line_of, 0xFF, sizeof prog->line_of);
   prog->first_code = -1;
   prog->last_word = -1;
}

void mano_program_free(ManoProgram* prog) {
   free(prog->src_buf);
   free(prog->lines);
   free(prog->line_addr);
   free(prog->path);
   mano_symtab_free(&prog->syms);
   mano_program_init(prog);
}

static uint32_t hash_name(const char* s) {
   uint32_t h = 2166136261u;
   for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
      h ^= *p;
      h *= 16777619u;
   }
   return h;
}

void mano_symtab_free(ManoSymTab* tab) {
   free(tab->items);
   free(tab->buckets);
   memset(tab, 0, sizeof *tab);
}

static void symtab_rehash(ManoSymTab* tab, size_t newcap) {
   int32_t* buckets = xmalloc(newcap * sizeof *buckets);
   memset(buckets, 0xFF, newcap * sizeof *buckets);

   size_t mask = newcap - 1;
   for (size_t i = 0; i < tab->count; i++) {
      size_t slot = (size_t)hash_name(tab->items[i].name) & mask;
      while (buckets[slot] >= 0)
         slot = (slot + 1) & mask;
      buckets[slot] = (int32_t)i;
   }

   free(tab->buckets);
   tab->buckets = buckets;
   tab->nbuckets = newcap;
}

int mano_symtab_find(const ManoSymTab* tab, const char* name) {
   if (!tab || !name || tab->nbuckets == 0)
      return -1;

   size_t mask = tab->nbuckets - 1;
   size_t slot = (size_t)hash_name(name) & mask;

   for (;;) {
      int32_t idx = tab->buckets[slot];
      if (idx < 0)
         return -1;
      if ((size_t)idx < tab->count &&
          strcmp(tab->items[idx].name, name) == 0)
         return tab->items[idx].addr;
      slot = (slot + 1) & mask;
   }
}

int mano_symtab_add(ManoSymTab* tab, const char* name, int addr) {
   if (!tab || !name)
      return -2;
   size_t nlen = strlen(name);
   if (nlen > MANO_MAX_NAME)
      return -2;

   uint32_t h = hash_name(name);

   if (tab->nbuckets == 0) {
      symtab_rehash(tab, 64);
   }
   else {
      size_t mask = tab->nbuckets - 1;
      size_t slot = (size_t)h & mask;
      for (;;) {
         int32_t idx = tab->buckets[slot];
         if (idx < 0)
            break;
         if ((size_t)idx < tab->count &&
             strcmp(tab->items[idx].name, name) == 0)
            return -1;
         slot = (slot + 1) & mask;
      }
      if ((tab->count + 1) * 4 >= tab->nbuckets * 3) {
         if (tab->nbuckets > SIZE_MAX / 2)
            return -2;
         symtab_rehash(tab, tab->nbuckets * 2);
      }
   }

   if (tab->count == tab->cap) {
      size_t ncap = tab->cap ? tab->cap * 2 : 32;
      tab->items = xreallocarray(tab->items, ncap, sizeof *tab->items);
      tab->cap = ncap;
   }

   size_t idx = tab->count;
   memcpy(tab->items[idx].name, name, nlen + 1);
   tab->items[idx].addr = addr;

   size_t mask = tab->nbuckets - 1;
   size_t slot = (size_t)h & mask;
   while (tab->buckets[slot] >= 0)
      slot = (slot + 1) & mask;
   tab->buckets[slot] = (int32_t)idx;
   tab->count++;

   return 0;
}
