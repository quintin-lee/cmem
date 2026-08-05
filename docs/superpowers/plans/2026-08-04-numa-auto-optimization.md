# cmem NUMA Auto-Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add automatic NUMA-aware allocation to cmem via a new `MP_FLAG_AUTO_NUMA` pool flag, thread-local-first node selection, and public topology query APIs — while fixing the currently-dead manual NUMA binding code.

**Architecture:** A lazily-initialized, atomically-guarded topology cache (`cpu_to_node[]` map + node count) is built once from `/sys/devices/system/node/online`, `/sys/devices/system/cpu/possible`, and per-node `cpulist` files. During allocation, `sys_mem_alloc` picks the node: manual `pool->numa_node` wins if set, otherwise `MP_FLAG_AUTO_NUMA` selects the current thread's node via `sched_getcpu()` + cache lookup, then applies `mbind(MPOL_BIND)`. On non-Linux, single-node systems, or mbind failure, the feature degrades silently to current behavior.

**Tech Stack:** C11 (strict `-std=c11 -D_GNU_SOURCE`), Linux syscalls (`SYS_mbind`, `sched_getcpu`), sysfs topology files, CMake+clang-tidy build, gcc Makefile build.

---

**Design spec:** `docs/superpowers/specs/2026-08-04-numa-auto-optimization-design.md` (committed `5d1ffb2`)

**CRITICAL BUILD/WORKFLOW RULES (from repo experience):**
- The CMake build auto-runs clang-tidy as a compile check. **ALWAYS run `clang-format -i <file>` before rebuilding.**
- Per-file verify: `cd build_cmake && ninja CMakeFiles/<target>.dir/<path>.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:` — empty output = clean.
- Full clean rebuild: `cd build_cmake && ninja -t clean && ninja` (capture warnings from the SAME pass).
- Full test suite: `ctest` + `./unit_tests` + `./advanced_tests` + stress test short build (`gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -DSTRESS_DURATION_SEC=15 -I./include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify`).
- `make format-check` must pass (clang-format --dry-run --Werror).
- Commit style: Conventional Commits with emoji, repo history uses `feat:`, `fix:`, `style:`, `docs:`, `chore:`. Do NOT push, do NOT amend.
- clang-tidy NOLINT placement: comments must sit on the EXACT line clang-tidy reports; after `clang-format -i` re-verify placement (formatting may move comments).
- Include order in test_main.c: `../include/cmem.h` FIRST, system headers, then `../include/cmem_override.h`.

---

## Phase 1: Fix dead manual NUMA binding

### Task 1.1: Make `SYS_mbind` available and define shared `CMEM_MPOL_BIND`

**Files:**
- Modify: `src/cmem_sys.c` (add `#include <sys/syscall.h>` near top includes, after `#include <sched.h>`)
- Modify: `src/cmem.c:41-45` (REMOVE the entire `#ifdef __linux__ #include <sys/syscall.h> #ifndef CMEM_MPOL_BIND #define CMEM_MPOL_BIND 2 #endif #endif` block — cmem.c uses syscall.h ONLY for this define)
- Modify: `src/cmem_internal.h` (add shared `#define CMEM_MPOL_BIND 2` in the constants region ~line 145)

- [ ] **Step 1: Add syscall.h include to cmem_sys.c**

Add after the existing includes (around line 21, after `#include <sched.h>`):

```c
#include <sys/syscall.h>
```

- [ ] **Step 2: Add CMEM_MPOL_BIND to cmem_internal.h**

In `src/cmem_internal.h`, in the constants region near `CMEM_FREED_MAGIC` (~line 145), add:

```c
#ifndef CMEM_MPOL_BIND
#define CMEM_MPOL_BIND 2 /* MPOL_BIND memory policy */
#endif
```

- [ ] **Step 3: Remove the dead define block from cmem.c**

Remove from `src/cmem.c` lines 41-45:

```c
#ifdef __linux__
#include <sys/syscall.h>
#ifndef CMEM_MPOL_BIND
#define CMEM_MPOL_BIND 2
#endif
#endif
```

- [ ] **Step 4: Verify compilation + zero warnings**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem
clang-format -i src/cmem.c src/cmem_sys.c src/cmem_internal.h
cd build_cmake && ninja CMakeFiles/cmem.dir/src/cmem.c.o CMakeFiles/cmem.dir/src/cmem_sys.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:
```
Expected: empty output (no warnings/errors).

- [ ] **Step 5: Verify the mbind block is now LIVE**

Run:
```bash
printf 'SYS_mbind\n' | gcc -dM -E -include /usr/include/sys/syscall.h - 2>/dev/null | grep SYS_mbind
```
Expected: `#define SYS_mbind __NR_mbind`. Also confirm `gcc -dM -E src/cmem_sys.c | grep MPOL_BIND` now shows `CMEM_MPOL_BIND 2`.

- [ ] **Step 6: Commit**

```bash
git add src/cmem.c src/cmem_sys.c src/cmem_internal.h
git commit -m "fix(numa): enable mbind syscall for manual NUMA binding"
```

---

### Task 1.2: Verify existing manual binding still works end-to-end

**Files:**
- Test: `tests/test_main.c` (existing `test_numa_node_binding` at lines 433-456)

- [ ] **Step 1: Build and run unit_tests**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem/build_cmake && ninja unit_tests && ./unit_tests
```
Expected: `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!` (the existing `test_numa_node_binding` calls `mp_set_numa_node(pool, 0)` — now actually performs mbind).

---

## Phase 2: Public API surface

### Task 2.1: Add `MP_FLAG_AUTO_NUMA` and query API declarations

**Files:**
- Modify: `include/cmem.h:97-101` (enum tail — add flag after `MP_FLAG_REPORT_LEAKS_ON_DESTROY`)
- Modify: `include/cmem.h` (add new API prototypes after `mp_set_numa_node` declaration ~line 300)

- [ ] **Step 1: Add the flag to mp_flags_t enum**

In `include/cmem.h`, after `MP_FLAG_REPORT_LEAKS_ON_DESTROY = (1 << 15)` add:

```c
    MP_FLAG_AUTO_NUMA = (1 << 16), /**< Automatically bind allocations to the calling thread's NUMA node */
```

(Keep `(1 << N)` int style to match existing members; the trailing comma is required now since it's no longer the last member.)

- [ ] **Step 2: Add query API prototypes**

After the `mp_set_numa_node` prototype (~line 300):

```c
/**
 * @brief Returns the number of detected NUMA nodes (>= 1).
 *
 * Returns 1 on single-node systems and on unsupported platforms
 * (non-Linux, or when sysfs topology is unavailable).
 *
 * @return NUMA node count, always >= 1
 */
int mp_numa_node_count(void);

/**
 * @brief Returns the NUMA node that owns the given CPU.
 *
 * @param cpu CPU index (0-based). Negative or out-of-range values return 0.
 * @return NUMA node id owning the CPU, or 0 on failure/unknown
 */
int mp_cpu_to_node(int cpu);
```

- [ ] **Step 3: Verify header compiles**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem && gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -I./include -fsyntax-only -x c tests/test_main.c
```
Expected: no errors (linker symbols not needed for syntax check).

- [ ] **Step 4: Commit**

```bash
git add include/cmem.h
git commit -m "feat(numa): add MP_FLAG_AUTO_NUMA and topology query APIs"
```

---

## Phase 3: Topology cache and probe implementation

### Task 3.1: Internal struct + externs in cmem_internal.h

**Files:**
- Modify: `src/cmem_internal.h` (add struct + extern declarations near line 577 area, in the cmem_sys.c extern section)

- [ ] **Step 1: Add internal topology struct + externs**

In `src/cmem_internal.h` near the existing `extern int cmem_sched_getcpu(void);` (line 577), add:

```c
typedef struct cmem_numa_topology {
    int  node_count;   /* number of NUMA nodes detected (>= 1) */
    int  cpu_count;    /* number of CPUs in the possible map */
    int *cpu_to_node;  /* array[cpu_count], maps cpu -> node id */
} cmem_numa_topology_t;

extern int cmem_numa_current_node(void);
extern int cmem_numa_node_count(void);
extern int cmem_cpu_to_node(int cpu);
```

- [ ] **Step 2: Commit**

```bash
git add src/cmem_internal.h
git commit -m "chore(numa): add internal NUMA topology structures"
```

---

### Task 3.2: Implement topology probe + cache + query APIs in cmem_sys.c

**Files:**
- Modify: `src/cmem_sys.c` (add `#include <unistd.h>` if missing, topology init + query functions near `cmem_sched_getcpu`)

- [ ] **Step 1: Add includes**

Ensure these includes exist at the top of `src/cmem_sys.c` (add any missing):
```c
#include <unistd.h>       /* access() */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
```

Also add the decimal-base constant right after the includes (guarded, since cmem_event.c defines its own file-local one):
```c
#ifndef CMEM_DECIMAL_BASE
#define CMEM_DECIMAL_BASE 10 /* base for strtol family */
#endif
```

- [ ] **Step 2: Implement the lazy topology cache**

Add after `cmem_sched_getcpu()` (near line 40). This is the COMPLETE reference implementation — copy verbatim:

```c
/* -------------------- NUMA topology detection -------------------- */

static cmem_numa_topology_t g_numa_topo;
static atomic_flag g_numa_topo_init = ATOMIC_FLAG_INIT;

/* Expand a sysfs list like "0-3,8" into an array of ids.
 * Returns number of ids written, or -1 on error. */
static int cmem_parse_idlist(const char *list, int *ids, int max_ids)
{
    if (!list || !ids || max_ids <= 0) {
        return -1;
    }

    int count = 0;
    const char *pos = list;
    while (*pos) {
        while (*pos == ',' || *pos == ' ' || *pos == '\n') {
            pos++;
        }
        if (!*pos) {
            break;
        }

        char *end = NULL;
        long low = strtol(pos, &end, CMEM_DECIMAL_BASE);
        if (end == pos || low < 0) {
            return -1;
        }
        long high = low;
        if (*end == '-') {
            high = strtol(end + 1, &end, CMEM_DECIMAL_BASE);
            if (high < low) {
                return -1;
            }
        }
        if (high - low + 1 > (long)(max_ids - count)) {
            high = low + (long)(max_ids - count) - 1;
        }
        for (long id = low; id <= high; id++) {
            ids[count++] = (int)id;
        }
        pos = end;
    }
    return count;
}

/* Build the topology cache. Runs once, guarded by g_numa_topo_init.
 * On any failure, falls back to node_count = 1 with no cpu map. */
static void cmem_numa_probe(void)
{
    g_numa_topo.node_count = 1;
    g_numa_topo.cpu_count = 0;
    g_numa_topo.cpu_to_node = NULL;

#if defined(__linux__)
    char line[256];
    int node_ids[64];
    int cpu_ids[1024];

    /* Read the set of online NUMA nodes. */
    int node_count = 1;
    FILE *node_file = fopen("/sys/devices/system/node/online", "r");
    if (node_file) {
        if (fgets(line, sizeof(line), node_file)) {
            int parsed = cmem_parse_idlist(line, node_ids, 64);
            if (parsed > 0) {
                node_count = parsed;
            }
        }
        (void)fclose(node_file);
    }
    g_numa_topo.node_count = node_count;

    /* Read the set of possible CPUs. */
    int cpu_count = 0;
    FILE *cpu_file = fopen("/sys/devices/system/cpu/possible", "r");
    if (cpu_file) {
        if (fgets(line, sizeof(line), cpu_file)) {
            int parsed = cmem_parse_idlist(line, cpu_ids, 1024);
            if (parsed > 0) {
                cpu_count = parsed;
            }
        }
        (void)fclose(cpu_file);
    }
    if (cpu_count <= 0) {
        return; /* no cpu info; stay with default node_count = 1 */
    }
    g_numa_topo.cpu_count = cpu_count;

    /* Build the cpu -> node map from per-node cpulists. */
    int *map = (int *)calloc((size_t)cpu_count, sizeof(int));
    if (!map) {
        return;
    }
    for (int i = 0; i < node_count; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", node_ids[i]);
        FILE *node_cpu_file = fopen(path, "r");
        if (!node_cpu_file) {
            continue;
        }
        if (fgets(line, sizeof(line), node_cpu_file)) {
            int cpu_in_node[1024];
            int n = cmem_parse_idlist(line, cpu_in_node, 1024);
            for (int j = 0; j < n; j++) {
                if (cpu_in_node[j] >= 0 && cpu_in_node[j] < cpu_count) {
                    map[cpu_in_node[j]] = node_ids[i];
                }
            }
        }
        (void)fclose(node_cpu_file);
    }
    g_numa_topo.cpu_to_node = map;
#endif /* __linux__ */
}

int cmem_numa_node_count(void)
{
    if (!atomic_flag_test_and_set(&g_numa_topo_init)) {
        cmem_numa_probe(); /* first caller wins; others may see default briefly */
    }
    return g_numa_topo.node_count;
}

int cmem_cpu_to_node(int cpu)
{
    if (cpu < 0) {
        return 0;
    }
    cmem_numa_node_count(); /* ensure init */
    if (g_numa_topo.cpu_to_node && cpu < g_numa_topo.cpu_count) {
        return g_numa_topo.cpu_to_node[cpu];
    }
    return 0;
}

/* Return the NUMA node of the calling thread, or -1 if unknown/unsupported. */
int cmem_numa_current_node(void)
{
#if defined(__linux__) && !defined(__APPLE__)
    int cpu = cmem_sched_getcpu();
    if (cpu < 0) {
        return -1;
    }
    return cmem_cpu_to_node(cpu);
#else
    return -1;
#endif
}
```

> **Notes:** (a) The `atomic_flag` one-time-init has a benign race — a concurrent caller may briefly read the default `node_count = 1` before the probe completes; consequence is at most returning 1 instead of the real count once. (b) `node_ids[64]` and `cpu_ids[1024]` caps are generous (real systems have far fewer); ranges longer than the cap are truncated at the end (parse order). (c) 64/128/256/1024 are powers of 2 → exempt from readability-magic-numbers per `.clang-tidy` IgnorePowersOf2IntegerValues. (d) `CMEM_DECIMAL_BASE` avoids the magic-number `10` in strtol.

- [ ] **Step 3: Public wrappers `mp_numa_node_count` / `mp_cpu_to_node`**

Add at the end of `src/cmem_sys.c`:

```c
int mp_numa_node_count(void)
{
    return cmem_numa_node_count();
}

int mp_cpu_to_node(int cpu)
{
    return cmem_cpu_to_node(cpu);
}
```

- [ ] **Step 4: Verify + rebuild + commit**

Run a quick smoke check first (links the whole library against a tiny driver):
```bash
cd /data/home/quintin/workspace/source/c/cmem && cat > /tmp/numa_smoke.c <<'EOF'
#include <stdio.h>
#include "cmem.h"
int main(void) {
    printf("node_count=%d\n", mp_numa_node_count());
    printf("cpu0_node=%d\n", mp_cpu_to_node(0));
    printf("cpu_neg=%d\n", mp_cpu_to_node(-1));
    return 0;
}
EOF
gcc -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -I./include -o /tmp/numa_smoke /tmp/numa_smoke.c src/*.c -pthread && /tmp/numa_smoke; rm -f /tmp/numa_smoke /tmp/numa_smoke.c
```
Expected: `node_count=1` (or the real node count on NUMA hardware), `cpu0_node=0`, `cpu_neg=0`. No warnings/errors printed.

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem
clang-format -i src/cmem_sys.c
cd build_cmake && ninja CMakeFiles/cmem.dir/src/cmem_sys.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:
```
Expected: empty. Then:
```bash
git add src/cmem_sys.c
git commit -m "feat(numa): implement topology probe and query APIs"
```

---

## Phase 4: Wire auto-NUMA into allocation

### Task 4.1: sys_mem_alloc auto path

**Files:**
- Modify: `src/cmem_sys.c:183-190` (the mbind block in sys_mem_alloc)

- [ ] **Step 1: Replace the mbind block**

Replace the existing:
```c
#if defined(__linux__) && defined(SYS_mbind)
    if (pool && pool->numa_node >= 0 && ptr) {
        unsigned long nodemask = (1UL << pool->numa_node);
        syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
    }
#endif
```
with:
```c
#if defined(__linux__) && defined(SYS_mbind)
    if (pool && ptr) {
        int numa_node = pool->numa_node; /* manual binding wins */
        if (numa_node < 0 && (pool->flags & MP_FLAG_AUTO_NUMA)) {
            numa_node = cmem_numa_current_node(); /* thread-local-first */
        }
        if (numa_node >= 0) {
            unsigned long nodemask = (1UL << numa_node);
            syscall(SYS_mbind, ptr, size, CMEM_MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0);
        }
    }
#endif
```

- [ ] **Step 2: Verify + rebuild + commit**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem
clang-format -i src/cmem_sys.c
cd build_cmake && ninja CMakeFiles/cmem.dir/src/cmem_sys.c.o 2>&1 | grep -E 'warning:|error:' | grep -v note:
```
Expected: empty. Then:
```bash
git add src/cmem_sys.c
git commit -m "feat(numa): auto-bind allocations to calling thread's node"
```

---

## Phase 5: Tests

### Task 5.1: NUMA auto tests in test_main.c

**Files:**
- Modify: `tests/test_main.c` (add new test function after `test_numa_node_binding` at ~line 456; register in main() ~line 1430)
- Test: `tests/test_main.c`

- [ ] **Step 1: Add the new test function**

After `test_numa_node_binding` (~line 456), add:

```c
void test_numa_auto_optimization(void)
{
    /* Query API sanity. */
    assert(mp_numa_node_count() >= 1);
    assert(mp_cpu_to_node(-1) == 0);
    assert(mp_cpu_to_node(0) >= 0);
    assert(mp_cpu_to_node(0) < mp_numa_node_count());

    /* AUTO_NUMA pool: create, alloc, free, destroy. */
    memory_pool_t *pool = mp_create(0, MP_FLAG_AUTO_NUMA);
    assert(pool);
    void *p1 = mp_alloc(pool, 1024 * 1024);
    assert(p1);
    memset(p1, 0x77, 1024 * 1024);
    mp_free(pool, p1);
    assert(mp_check_leaks(pool));
    mp_destroy(pool);

    /* Manual binding still wins over auto. */
    memory_pool_t *pool2 = mp_create(0, MP_FLAG_AUTO_NUMA);
    assert(pool2);
    assert(mp_set_numa_node(pool2, 0) == true);
    void *p2 = mp_alloc(pool2, 4096);
    assert(p2);
    mp_free(pool2, p2);
    mp_destroy(pool2);

    TEST_PASS("test_numa_auto_optimization");
}
```

- [ ] **Step 2: Register in main()**

In `main()` after the `test_numa_node_binding();` call, add:
```c
    test_numa_auto_optimization();
```

- [ ] **Step 3: Build + run**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem
clang-format -i tests/test_main.c
cd build_cmake && ninja unit_tests 2>&1 | grep -E 'warning:|error:' | grep -v note: && ./unit_tests
```
Expected: no warnings; `ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!`.

- [ ] **Step 4: Commit**

```bash
git add tests/test_main.c
git commit -m "test(numa): add auto-NUMA optimization tests"
```

---

## Phase 6: Documentation

### Task 6.1: Update performance.md (en + zh) and CHANGELOG

**Files:**
- Modify: `docs/en/performance.md` (add §7.4 Auto-NUMA after 7.3)
- Modify: `docs/zh/performance.md` (same, Chinese)
- Modify: `CHANGELOG.md` (Unreleased → Added)

- [ ] **Step 1: Add §7.4 Auto-NUMA to docs/en/performance.md**

After §7.3 Multi-Node Scenario, add:

````markdown
### 7.4 Automatic NUMA (Auto-NUMA)

Create a pool with `MP_FLAG_AUTO_NUMA` to automatically bind each allocation to
the NUMA node of the calling thread:

```c
memory_pool_t* pool = mp_create(0, MP_FLAG_AUTO_NUMA);
```

- Node selection: `sched_getcpu()` → CPU-to-node map → `mbind(MPOL_BIND)`.
- Manual `mp_set_numa_node(pool, node)` always overrides automatic selection.
- Query APIs: `mp_numa_node_count()` returns the detected node count (>= 1);
  `mp_cpu_to_node(cpu)` returns the node owning a CPU.
- Degradation: on non-Linux, single-node systems, or when `mbind` fails, the
  pool silently falls back to non-bound allocation.
````

- [ ] **Step 2: Mirror to docs/zh/performance.md** (translate the same content to Chinese).

- [ ] **Step 3: Update CHANGELOG.md**

In `## [Unreleased]` → `### Added`, add:
```markdown
- `MP_FLAG_AUTO_NUMA` pool flag with thread-local-first NUMA binding, plus `mp_numa_node_count()` / `mp_cpu_to_node()` topology query APIs
```

- [ ] **Step 4: Commit**

```bash
git add docs/en/performance.md docs/zh/performance.md CHANGELOG.md
git commit -m "docs(numa): document auto-NUMA optimization"
```

---

## Phase 7: Full verification

### Task 7.1: Clean rebuild + full test suite

- [ ] **Step 1: Full clean rebuild**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem/build_cmake && ninja -t clean && ninja 2>&1 | tee /tmp/final_build.log
grep -cE 'warning:|error:' /tmp/final_build.log
```
Expected: `0`.

- [ ] **Step 2: Full test suite**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem/build_cmake && ctest 2>&1 | tail -5
```
Expected: 100% tests passed, 3/3.

Run each binary:
```bash
./unit_tests   # ALL CMEM UNIT TESTS PASSED SUCCESSFULLY!
./advanced_tests
./cpp_tests
```

- [ ] **Step 3: Stress test short build**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem && gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -DSTRESS_DURATION_SEC=15 -I./include tests/stress_test.c src/*.c -pthread -o /tmp/stress_verify && /tmp/stress_verify; rm -f /tmp/stress_verify
```
Expected: `[STRESS] Long-run stress test completed successfully.` exit 0.

- [ ] **Step 4: format-check + git clean**

Run:
```bash
cd /data/home/quintin/workspace/source/c/cmem && make format-check && git status --porcelain
```
Expected: format-check exit 0; git status shows ONLY expected modified files (no unexpected changes).

---

**DONE — verify final `git log --oneline -10` shows the 7 feature commits on top of `5d1ffb2` (spec), working tree clean.**
