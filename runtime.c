/**
 * runtime.c
 *
 * Runtime library for the PTA validator.
 * Linked with the instrumented program.
 *
 * Responsibilities:
 *   1. __pta_init         — parse the points-to file, build Structure 1
 *   2. __pta_record_alloc — populate Structure 2 (abstract name -> live allocations)
 *   3. __pta_record_free  — remove an allocation from Structure 2
 *   4. __pta_check_deref  — at each pointer dereference, validate against
 *                           the points-to map and live allocation ranges
 *
 * Data structures (in-memory, C implementation):
 *
 *   Structure 1 (static, from pta file):
 *     A hash map:  pointer_name (string) -> list of pointee_names (strings)
 *
 *   Structure 2 (dynamic, updated at runtime):
 *     A hash map:  abstract_name (string) -> list of (base_addr, size) ranges
 *
 * This file uses only C99 + POSIX. No C++ required.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define MAX_NAME_LEN       256
#define HASH_TABLE_SIZE    1024   // Must be power of 2
#define MAX_POINTEES       64     // Max pointees per pointer variable
#define MAX_ALLOC_RANGES   256    // Max live allocations per abstract variable

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

typedef struct AllocRange {
  uintptr_t base;
  size_t    size;
} AllocRange;

// Entry in Structure 2: abstract name -> list of live allocations
typedef struct AllocEntry {
  char        name[MAX_NAME_LEN];
  AllocRange  ranges[MAX_ALLOC_RANGES];
  int         num_ranges;
  struct AllocEntry *next; // chaining for hash collisions
} AllocEntry;

// Entry in Structure 1: pointer name -> list of pointee names
typedef struct PtaEntry {
  char  ptr_name[MAX_NAME_LEN];
  char  pointees[MAX_POINTEES][MAX_NAME_LEN];
  int   num_pointees;
  struct PtaEntry *next;
} PtaEntry;

// ---------------------------------------------------------------------------
// Hash tables
// ---------------------------------------------------------------------------

static PtaEntry  *pta_table[HASH_TABLE_SIZE];   // Structure 1
static AllocEntry *alloc_table[HASH_TABLE_SIZE]; // Structure 2

static int runtime_initialized = 0;

// ---------------------------------------------------------------------------
// Simple djb2 hash
// ---------------------------------------------------------------------------

static unsigned int hash_str(const char *s) {
  unsigned long h = 5381;
  int c;
  while ((c = *s++))
    h = ((h << 5) + h) + (unsigned long)c;
  return (unsigned int)(h & (HASH_TABLE_SIZE - 1));
}

// ---------------------------------------------------------------------------
// Structure 1 helpers
// ---------------------------------------------------------------------------

static PtaEntry *pta_find(const char *ptr_name) {
  unsigned int h = hash_str(ptr_name);
  PtaEntry *e = pta_table[h];
  while (e) {
    if (strncmp(e->ptr_name, ptr_name, MAX_NAME_LEN) == 0)
      return e;
    e = e->next;
  }
  return NULL;
}

static PtaEntry *pta_insert(const char *ptr_name) {
  unsigned int h = hash_str(ptr_name);
  PtaEntry *e = calloc(1, sizeof(PtaEntry));
  if (!e) { perror("calloc"); exit(1); }
  strncpy(e->ptr_name, ptr_name, MAX_NAME_LEN - 1);
  e->next = pta_table[h];
  pta_table[h] = e;
  return e;
}

// ---------------------------------------------------------------------------
// Structure 2 helpers
// ---------------------------------------------------------------------------

static AllocEntry *alloc_find(const char *name) {
  unsigned int h = hash_str(name);
  AllocEntry *e = alloc_table[h];
  while (e) {
    if (strncmp(e->name, name, MAX_NAME_LEN) == 0)
      return e;
    e = e->next;
  }
  return NULL;
}

static AllocEntry *alloc_find_or_create(const char *name) {
  AllocEntry *e = alloc_find(name);
  if (e) return e;
  unsigned int h = hash_str(name);
  e = calloc(1, sizeof(AllocEntry));
  if (!e) { perror("calloc"); exit(1); }
  strncpy(e->name, name, MAX_NAME_LEN - 1);
  e->next = alloc_table[h];
  alloc_table[h] = e;
  return e;
}

// ---------------------------------------------------------------------------
// Points-to file parser
//
// Expected format (same as PtaValidator.cpp):
//   # comment
//   %p -> %x %y
//   @gptr -> @ga
// ---------------------------------------------------------------------------

static char *trim(char *s) {
  // ltrim
  while (*s == ' ' || *s == '\t') s++;
  // rtrim
  char *end = s + strlen(s) - 1;
  while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
    *end-- = '\0';
  return s;
}

static void parse_pta_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "[PtaRuntime] ERROR: cannot open points-to file: %s\n", path);
    exit(1);
  }

  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    char *l = trim(line);
    if (l[0] == '\0' || l[0] == '#')
      continue;

    // Find "->"
    char *arrow = strstr(l, "->");
    if (!arrow) continue;

    // Split into LHS and RHS
    *arrow = '\0';
    char *lhs = trim(l);
    char *rhs = trim(arrow + 2);

    if (strlen(lhs) == 0) continue;

    // Find or create entry for the pointer
    PtaEntry *entry = pta_find(lhs);
    if (!entry) entry = pta_insert(lhs);

    // Tokenize RHS by spaces
    char *tok = strtok(rhs, " \t");
    while (tok) {
      if (entry->num_pointees < MAX_POINTEES) {
        strncpy(entry->pointees[entry->num_pointees], tok, MAX_NAME_LEN - 1);
        entry->num_pointees++;
      } else {
        fprintf(stderr,
                "[PtaRuntime] WARNING: too many pointees for '%s', truncating.\n",
                lhs);
      }
      tok = strtok(NULL, " \t");
    }
  }

  fclose(f);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * __pta_init — called once at program startup (via global constructor).
 * Reads the points-to file and populates Structure 1.
 */
void __pta_init(const char *pta_file_path) {
  if (runtime_initialized) return;
  memset(pta_table, 0, sizeof(pta_table));
  memset(alloc_table, 0, sizeof(alloc_table));
  parse_pta_file(pta_file_path);
  runtime_initialized = 1;
  fprintf(stderr, "[PtaRuntime] Initialized from: %s\n", pta_file_path);
}

/**
 * __pta_record_alloc — called after every alloca/malloc/calloc.
 * Records the (base, size) range for the given abstract variable name
 * in Structure 2.
 *
 * @param abstract_name  The IR-level name of the allocated variable (e.g. "%x")
 * @param ptr            The actual runtime address
 * @param size           The size of the allocation in bytes
 */
void __pta_record_alloc(const char *abstract_name, void *ptr, size_t size) {
  if (!runtime_initialized) return;

  AllocEntry *e = alloc_find_or_create(abstract_name);

  // Check if this exact base is already recorded (re-use of SSA value)
  uintptr_t base = (uintptr_t)ptr;
  for (int i = 0; i < e->num_ranges; i++) {
    if (e->ranges[i].base == base) {
      // Update size in case of realloc-like patterns
      e->ranges[i].size = size;
      return;
    }
  }

  if (e->num_ranges >= MAX_ALLOC_RANGES) {
    fprintf(stderr,
            "[PtaRuntime] WARNING: too many live allocations for '%s'.\n",
            abstract_name);
    return;
  }

  e->ranges[e->num_ranges].base = base;
  e->ranges[e->num_ranges].size = size;
  e->num_ranges++;
}

/**
 * __pta_record_free — called before every free().
 * Removes the allocation range whose base matches ptr from Structure 2.
 *
 * @param ptr  The pointer being freed
 */
void __pta_record_free(void *ptr) {
  if (!runtime_initialized || !ptr) return;

  uintptr_t base = (uintptr_t)ptr;

  // Scan all entries in Structure 2 looking for this base address
  for (int i = 0; i < HASH_TABLE_SIZE; i++) {
    for (AllocEntry *e = alloc_table[i]; e; e = e->next) {
      for (int j = 0; j < e->num_ranges; j++) {
        if (e->ranges[j].base == base) {
          // Swap with last and shrink
          e->ranges[j] = e->ranges[e->num_ranges - 1];
          e->num_ranges--;
          return;
        }
      }
    }
  }
}

/**
 * __pta_check_deref — called before every load/store through a pointer.
 * Validates that the address being dereferenced falls within the union of
 * allocation ranges of the pointer's pointees (per the points-to map).
 *
 * @param ptr_var_name  The IR-level name of the pointer variable (e.g. "%p")
 * @param addr          The runtime address being dereferenced
 */
void __pta_check_deref(const char *ptr_var_name, void *addr) {
  if (!runtime_initialized) return;

  // Look up the pointer in Structure 1
  PtaEntry *pta = pta_find(ptr_var_name);
  if (!pta) {
    // Not in the points-to map at all — skip (conservative: don't flag)
    return;
  }

  uintptr_t deref_addr = (uintptr_t)addr;

  // For each pointee in the points-to set, check if deref_addr falls
  // within any of its live allocation ranges
  for (int i = 0; i < pta->num_pointees; i++) {
    const char *pointee_name = pta->pointees[i];
    AllocEntry *ae = alloc_find(pointee_name);

    if (!ae) {
      // No allocation recorded for this pointee yet — this could mean
      // the pointee is a global not yet recorded, or a bug.
      // We treat this as "no range available" and continue checking others.
      continue;
    }

    for (int j = 0; j < ae->num_ranges; j++) {
      uintptr_t base = ae->ranges[j].base;
      size_t    size = ae->ranges[j].size;
      if (deref_addr >= base && deref_addr < base + size) {
        // VALID: address falls within a known pointee's allocation
        return;
      }
    }
  }

  // None of the pointee ranges covered this address — UNSOUND!
  fprintf(stderr,
          "[PtaRuntime] UNSOUND: pointer '%s' dereferenced address %p, "
          "which is not within any expected pointee allocation.\n"
          "             Expected pointees: { ",
          ptr_var_name, addr);

  for (int i = 0; i < pta->num_pointees; i++) {
    fprintf(stderr, "%s ", pta->pointees[i]);
    AllocEntry *ae = alloc_find(pta->pointees[i]);
    if (ae && ae->num_ranges > 0) {
      fprintf(stderr, "[");
      for (int j = 0; j < ae->num_ranges; j++) {
        fprintf(stderr, "%p+%zu", (void *)ae->ranges[j].base,
                ae->ranges[j].size);
        if (j + 1 < ae->num_ranges) fprintf(stderr, ", ");
      }
      fprintf(stderr, "] ");
    } else {
      fprintf(stderr, "[no live alloc] ");
    }
  }
  fprintf(stderr, "}\n");

  // Exit with failure to clearly signal unsoundness
  // (change to a counter increment if you want to collect all violations)
  exit(1);
}