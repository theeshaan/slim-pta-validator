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
 *   4. __pta_mark_ptr_initialized — record initialized pointer-storage slots
 *   5. __pta_check_deref  — at each pointer dereference, validate against
 *                           the points-to map and live allocation ranges
 *
 * Data structures (in-memory, C implementation):
 *
 *   Structure 1 (static, from pta file):
 *     FI mode: pointer_name -> list of pointee_names
 *     FS mode: (program_point, pointer_name) -> list of pointee_names
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
#include <limits.h>

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

typedef struct FsPtaEntry {
  char program_point[MAX_NAME_LEN];
  char ptr_name[MAX_NAME_LEN];
  char pointees[MAX_POINTEES][MAX_NAME_LEN];
  int num_pointees;
  struct FsPtaEntry *next;
} FsPtaEntry;

typedef struct PtrInitEntry {
  uintptr_t addr;
  struct PtrInitEntry *next;
} PtrInitEntry;

typedef struct ProgramPointOrderEntry {
  char program_point[MAX_NAME_LEN];
  uint64_t ordinal;
  struct ProgramPointOrderEntry *next;
} ProgramPointOrderEntry;

typedef enum PtaMode {
  PTA_MODE_FI = 0,
  PTA_MODE_FS = 1,
} PtaMode;

typedef enum ProgramPointFormat {
  PROGRAM_POINT_IR = 0,
  PROGRAM_POINT_SOURCE_LINE = 1,
} ProgramPointFormat;

// ---------------------------------------------------------------------------
// Hash tables
// ---------------------------------------------------------------------------

static PtaEntry  *pta_table[HASH_TABLE_SIZE];    // FI Structure 1
static FsPtaEntry *fs_pta_table[HASH_TABLE_SIZE]; // FS Structure 1
static AllocEntry *alloc_table[HASH_TABLE_SIZE]; // Structure 2
static PtrInitEntry *ptr_init_table[HASH_TABLE_SIZE]; // Initialized pointer slots
static ProgramPointOrderEntry *program_point_order_table[HASH_TABLE_SIZE];

static int runtime_initialized = 0;
static unsigned long unsound_count = 0;
static PtaMode active_mode = PTA_MODE_FI;
static ProgramPointFormat active_program_point_format = PROGRAM_POINT_IR;

static void report_validation_summary(void) {
  if (!runtime_initialized)
    return;

  if (unsound_count == 0) {
    fprintf(stderr,
            "[PtaRuntime] SUCCESS: no UNSOUND dereference observed during "
            "execution.\n");
  } else {
    fprintf(stderr,
            "[PtaRuntime] FAILURE: observed %lu UNSOUND dereference(s) during "
            "execution.\n",
            unsound_count);
  }
}

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

static unsigned int hash_fs_key(const char *program_point, const char *ptr_name) {
  unsigned long h = 5381u;
  int c;
  while ((c = *program_point++))
    h = ((h << 5) + h) + (unsigned long)c;
  while ((c = *ptr_name++))
    h = ((h << 5) + h) + (unsigned long)c;
  return (unsigned int)(h & (HASH_TABLE_SIZE - 1));
}

static unsigned int hash_addr(uintptr_t addr) {
  addr ^= addr >> 33;
  addr *= 0xff51afd7ed558ccdULL;
  addr ^= addr >> 33;
  return (unsigned int)(addr & (HASH_TABLE_SIZE - 1));
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

static FsPtaEntry *fs_pta_find(const char *program_point, const char *ptr_name) {
  unsigned int h = hash_fs_key(program_point, ptr_name);
  FsPtaEntry *e = fs_pta_table[h];
  while (e) {
    if (strncmp(e->program_point, program_point, MAX_NAME_LEN) == 0 &&
        strncmp(e->ptr_name, ptr_name, MAX_NAME_LEN) == 0)
      return e;
    e = e->next;
  }
  return NULL;
}

static ProgramPointOrderEntry *program_point_order_find(const char *program_point) {
  unsigned int h = hash_str(program_point);
  ProgramPointOrderEntry *e = program_point_order_table[h];
  while (e) {
    if (strncmp(e->program_point, program_point, MAX_NAME_LEN) == 0)
      return e;
    e = e->next;
  }
  return NULL;
}

static int parse_source_line_number(const char *program_point) {
  char *end;
  long value;

  if (!program_point || program_point[0] == '\0')
    return -1;

  value = strtol(program_point, &end, 10);
  if (*end != '\0' || value < 0 || value > INT32_MAX)
    return -1;

  return (int)value;
}

static FsPtaEntry *fs_pta_find_latest_source_line(const char *program_point,
                                                  const char *ptr_name) {
  int current_line = parse_source_line_number(program_point);
  FsPtaEntry *best = NULL;
  int best_line = -1;

  if (current_line < 0)
    return NULL;

  for (int i = 0; i < HASH_TABLE_SIZE; i++) {
    for (FsPtaEntry *e = fs_pta_table[i]; e; e = e->next) {
      int fact_line;

      if (strncmp(e->ptr_name, ptr_name, MAX_NAME_LEN) != 0)
        continue;

      fact_line = parse_source_line_number(e->program_point);
      if (fact_line < 0 || fact_line > current_line)
        continue;

      if (!best || fact_line > best_line) {
        best = e;
        best_line = fact_line;
      }
    }
  }

  return best;
}

static FsPtaEntry *fs_pta_find_latest_ir(const char *ptr_name,
                                         uint64_t current_ordinal) {
  FsPtaEntry *best = NULL;
  uint64_t best_ordinal = 0;

  for (int i = 0; i < HASH_TABLE_SIZE; i++) {
    for (FsPtaEntry *e = fs_pta_table[i]; e; e = e->next) {
      ProgramPointOrderEntry *order_entry;

      if (strncmp(e->ptr_name, ptr_name, MAX_NAME_LEN) != 0)
        continue;

      order_entry = program_point_order_find(e->program_point);
      if (!order_entry || order_entry->ordinal > current_ordinal)
        continue;

      if (!best || order_entry->ordinal > best_ordinal) {
        best = e;
        best_ordinal = order_entry->ordinal;
      }
    }
  }

  return best;
}

static FsPtaEntry *fs_pta_insert(const char *program_point, const char *ptr_name) {
  unsigned int h = hash_fs_key(program_point, ptr_name);
  FsPtaEntry *e = calloc(1, sizeof(FsPtaEntry));
  if (!e) { perror("calloc"); exit(1); }
  strncpy(e->program_point, program_point, MAX_NAME_LEN - 1);
  strncpy(e->ptr_name, ptr_name, MAX_NAME_LEN - 1);
  e->next = fs_pta_table[h];
  fs_pta_table[h] = e;
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

static int ptr_init_contains(uintptr_t addr) {
  unsigned int h = hash_addr(addr);
  PtrInitEntry *e = ptr_init_table[h];
  while (e) {
    if (e->addr == addr)
      return 1;
    e = e->next;
  }
  return 0;
}

static void ptr_init_insert(uintptr_t addr) {
  unsigned int h = hash_addr(addr);
  PtrInitEntry *e = calloc(1, sizeof(PtrInitEntry));
  if (!e) { perror("calloc"); exit(1); }
  e->addr = addr;
  e->next = ptr_init_table[h];
  ptr_init_table[h] = e;
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
  if (*s == '\0')
    return s;
  char *end = s + strlen(s) - 1;
  while (end >= s &&
         (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
    *end-- = '\0';
  return s;
}

static void strip_comment(char *line) {
  char *comment = strchr(line, '#');
  if (comment)
    *comment = '\0';
}

static void parse_fi_pta_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "[PtaRuntime] ERROR: cannot open points-to file: %s\n", path);
    exit(1);
  }

  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    strip_comment(line);
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
    entry->num_pointees = 0;

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

static void parse_fs_pta_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "[PtaRuntime] ERROR: cannot open points-to file: %s\n", path);
    exit(1);
  }

  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    strip_comment(line);
    char *l = trim(line);
    if (l[0] == '\0')
      continue;

    char *arrow = strstr(l, "->");
    if (!arrow)
      continue;

    *arrow = '\0';
    char *lhs = trim(l);
    char *rhs = trim(arrow + 2);
    if (lhs[0] != '@')
      continue;

    char *program_point_tok = strtok(lhs, " \t");
    char *ptr_tok = strtok(NULL, " \t");
    if (!program_point_tok || !ptr_tok || program_point_tok[0] != '@')
      continue;
    if (program_point_tok[1] == '\0')
      continue;

    const char *program_point = program_point_tok + 1;
    FsPtaEntry *entry = fs_pta_find(program_point, ptr_tok);
    if (!entry)
      entry = fs_pta_insert(program_point, ptr_tok);
    entry->num_pointees = 0;

    char *tok = strtok(rhs, " \t");
    while (tok) {
      if (entry->num_pointees < MAX_POINTEES) {
        strncpy(entry->pointees[entry->num_pointees], tok, MAX_NAME_LEN - 1);
        entry->num_pointees++;
      } else {
        fprintf(stderr,
                "[PtaRuntime] WARNING: too many FS pointees for '@%s %s', truncating.\n",
                program_point, ptr_tok);
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
void __pta_init(const char *pta_file_path, uint64_t pta_mode,
                uint64_t program_point_format) {
  if (runtime_initialized) return;
  memset(pta_table, 0, sizeof(pta_table));
  memset(fs_pta_table, 0, sizeof(fs_pta_table));
  memset(alloc_table, 0, sizeof(alloc_table));
  memset(ptr_init_table, 0, sizeof(ptr_init_table));
  memset(program_point_order_table, 0, sizeof(program_point_order_table));
  unsound_count = 0;
  active_mode = (pta_mode == PTA_MODE_FS) ? PTA_MODE_FS : PTA_MODE_FI;
  active_program_point_format =
      (program_point_format == PROGRAM_POINT_SOURCE_LINE)
          ? PROGRAM_POINT_SOURCE_LINE
          : PROGRAM_POINT_IR;
  if (active_mode == PTA_MODE_FS)
    parse_fs_pta_file(pta_file_path);
  else
    parse_fi_pta_file(pta_file_path);
  runtime_initialized = 1;
  atexit(report_validation_summary);
  fprintf(stderr, "[PtaRuntime] Initialized from: %s (mode=%s, program-point-format=%s)\n",
          pta_file_path, active_mode == PTA_MODE_FS ? "fs" : "fi",
          active_program_point_format == PROGRAM_POINT_SOURCE_LINE
              ? "source-line"
              : "ir");
}

void __pta_register_program_point(const char *program_point, uint64_t ordinal) {
  unsigned int h;
  ProgramPointOrderEntry *e;

  if (!runtime_initialized || !program_point)
    return;

  e = program_point_order_find(program_point);
  if (e) {
    e->ordinal = ordinal;
    return;
  }

  h = hash_str(program_point);
  e = calloc(1, sizeof(ProgramPointOrderEntry));
  if (!e) { perror("calloc"); exit(1); }
  strncpy(e->program_point, program_point, MAX_NAME_LEN - 1);
  e->ordinal = ordinal;
  e->next = program_point_order_table[h];
  program_point_order_table[h] = e;
}

void __pta_mark_ptr_initialized(void *slot_addr) {
  uintptr_t addr;

  if (!runtime_initialized || !slot_addr)
    return;

  addr = (uintptr_t)slot_addr;
  if (ptr_init_contains(addr))
    return;

  ptr_init_insert(addr);
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
 * @param program_point The IR-derived key "function:block:instruction_index"
 */
static int validate_against_pointees(const char *ptr_var_name,
                                     const char pointees[MAX_POINTEES][MAX_NAME_LEN],
                                     int num_pointees, void *addr) {
  uintptr_t deref_addr = (uintptr_t)addr;

  for (int i = 0; i < num_pointees; i++) {
    const char *pointee_name = pointees[i];
    AllocEntry *ae = alloc_find(pointee_name);

    if (!ae)
      continue;

    for (int j = 0; j < ae->num_ranges; j++) {
      uintptr_t base = ae->ranges[j].base;
      size_t size = ae->ranges[j].size;
      if (deref_addr >= base && deref_addr < base + size)
        return 1;
    }
  }

  return 0;
}

static void report_unsound(const char *ptr_var_name, void *addr,
                           const char *program_point,
                           const char *reason,
                           const char pointees[MAX_POINTEES][MAX_NAME_LEN],
                           int num_pointees) {
  unsound_count++;
  fprintf(stderr,
          "[PtaRuntime] UNSOUND: pointer '%s' at program point '@%s' dereferenced address %p (%s).\n",
          ptr_var_name, program_point, addr, reason);
  fprintf(stderr, "             Expected pointees: { ");
  for (int i = 0; i < num_pointees; i++) {
    fprintf(stderr, "%s ", pointees[i]);
    AllocEntry *ae = alloc_find(pointees[i]);
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
}

static void report_uninitialized_deref(const char *ptr_var_name, void *addr,
                                       const char *program_point,
                                       void *ptr_source_addr) {
  unsound_count++;
  fprintf(stderr,
          "[PtaRuntime] UNSOUND: uninitialized dereference encountered for pointer '%s' at program point '@%s' (loaded from slot %p, dereferenced address %p).\n",
          ptr_var_name, program_point, ptr_source_addr, addr);
}

void __pta_check_deref(const char *ptr_var_name, void *addr,
                       const char *program_point,
                       uint64_t program_point_ordinal,
                       void *ptr_source_addr) {
  if (!runtime_initialized) return;

  if (ptr_source_addr && !ptr_init_contains((uintptr_t)ptr_source_addr)) {
    report_uninitialized_deref(ptr_var_name, addr, program_point,
                               ptr_source_addr);
    return;
  }

  if (active_mode == PTA_MODE_FS) {
    FsPtaEntry *pta =
        active_program_point_format == PROGRAM_POINT_SOURCE_LINE
            ? fs_pta_find_latest_source_line(program_point, ptr_var_name)
            : fs_pta_find_latest_ir(ptr_var_name, program_point_ordinal);
    if (!pta) {
      report_unsound(ptr_var_name, addr, program_point,
                     "no flow-sensitive points-to fact", NULL, 0);
      return;
    }
    if (pta->num_pointees == 0) {
      report_unsound(ptr_var_name, addr, program_point,
                     "empty flow-sensitive points-to set",
                     pta->pointees, pta->num_pointees);
      return;
    }
    if (validate_against_pointees(ptr_var_name, pta->pointees, pta->num_pointees,
                                  addr))
      return;

    report_unsound(ptr_var_name, addr, program_point,
                   "address is not within any expected pointee allocation",
                   pta->pointees, pta->num_pointees);
    return;
  }

  PtaEntry *pta = pta_find(ptr_var_name);
  if (!pta)
    return;
  if (validate_against_pointees(ptr_var_name, pta->pointees, pta->num_pointees,
                                addr))
    return;

  report_unsound(ptr_var_name, addr, program_point,
                 "address is not within any expected pointee allocation",
                 pta->pointees, pta->num_pointees);
}
