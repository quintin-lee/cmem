# Development and Contribution Guide

## Table of Contents

1. [Development Environment Setup](#1-development-environment-setup)
2. [Code Standards](#2-code-standards)
3. [Build System](#3-build-system)
4. [Testing Guide](#4-testing-guide)
5. [Commit Conventions](#5-commit-conventions)
6. [Branch Strategy](#6-branch-strategy)
7. [Documentation Writing Standards](#7-documentation-writing-standards)
8. [Performance Tuning Tips](#8-performance-tuning-tips)
9. [Debugging Tips](#9-debugging-tips)
10. [FAQ](#10-faq)

---

## 1. Development Environment Setup

### 1.1 Required Tools

| Tool | Minimum Version | Recommended Version | Purpose |
| :--- | :--- | :--- | :--- |
| GCC | 7.0 | 13.0+ | C/C++ compilation |
| Clang | 6.0 | 17.0+ | Alternative compiler |
| Make | 3.81 | 4.3+ | Build |
| CMake | 3.10 | 3.28+ | Optional build |
| Ninja | 1.10 | 1.11+ | Optional CMake generator |
| Git | 2.20 | 2.45+ | Version control |
| Python | 3.8 | 3.12+ | Scripts/tools |
| Doxygen | 1.8 | 1.9+ | API documentation generation |

### 1.2 Ubuntu/Debian Installation

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 doxygen
```

### 1.3 Fedora/RHEL Installation

```bash
sudo dnf install gcc gcc-c++ cmake ninja-build git python3 doxygen
```

### 1.4 macOS Installation

```bash
brew install gcc cmake ninja git python3 doxygen
```

---

## 2. Code Standards

### 2.1 C Code Standards

- **Standard**: C11 (`-std=c11`)
- **Compiler Warnings**: `-Wall -Wextra -Wpedantic -Werror`
- **Indentation**: 4 spaces
- **Naming**:
  - Functions: `snake_case`
  - Structs: `snake_case` + `_t` suffix
  - Macros: `UPPER_SNAKE_CASE`
  - Local variables: `snake_case`
- **Comments**: Doxygen style (`@brief`, `@param`, `@return`)

### 2.2 C++ Code Standards

- **Standard**: C++17 (`-std=c++17`)
- **Namespace**: `cmem`
- **Class Names**: `PascalCase`
- **Method Names**: `snake_case`
- **Template Parameters**: `PascalCase` (e.g., `typename T`)

### 2.3 Comment Standards

All public APIs must include Doxygen comments:

```c
/**
 * @brief Brief description (one line)
 *
 * Detailed description (optional, multi-line)
 *
 * @param pool Memory pool pointer
 * @param size Number of bytes requested
 * @return Returns pointer on success, NULL on failure
 */
void* mp_alloc(memory_pool_t* pool, size_t size);
```

### 2.4 Header Guard

```c
#ifndef CMEM_H
#define CMEM_H

// ... content ...

#endif // CMEM_H
```

---

## 3. Build System

### 3.1 Makefile Targets

```bash
make lib          # Compile static library build/libcmem.a
make test         # Compile and run C unit tests (Debug + Sanitizers)
make test_cpp     # Compile and run C++ tests
make bench        # Compile and run performance benchmarks
make examples     # Compile and run example programs
make clean        # Clean build artifacts
make all          # Compile library + tests + benchmarks + examples
```

### 3.2 CMake Build

```bash
# Debug build (with Sanitizers)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# Release build
cmake -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release

# Install
cmake --install build_release --prefix /usr/local
```

### 3.3 Compiler Flags

**Release (Production):**
```bash
-O3 -march=native -flto -DNDEBUG
```

**Debug (Development/Testing):**
```bash
-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## 4. Testing Guide

### 4.1 Test Structure

```
tests/
├── test_main.c         # C comprehensive unit tests (35+ test cases)
└── test_cpp.cpp        # C++ PMR + STL Allocator tests
```

### 4.2 Adding a New Test

Add a test function in `tests/test_main.c`:

```c
static void test_my_new_feature(void) {
    printf("--- Test: My New Feature ---\n");
    
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    
    // Test logic
    void* p = mp_alloc(pool, 128);
    assert(p != NULL);
    
    // Cleanup
    mp_free(pool, p);
    mp_destroy(pool);
    
    printf("[PASS] test_my_new_feature\n\n");
}
```

Then call it in the `main()` function:

```c
int main() {
    test_my_new_feature();
    // ... other tests
    return 0;
}
```

### 4.3 Running Tests

```bash
# C unit tests
make test

# C++ tests
make test_cpp

# With AddressSanitizer
make test

# With Valgrind
valgrind --leak-check=full ./build/unit_tests
```

### 4.4 Test Coverage Check

```bash
# Using gcov
gcov src/cmem.c

# Using lcov
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

---

## 5. Commit Conventions

### 5.1 Gitmoji Convention

Gitmoji is used as the commit message prefix:

| Emoji | Type | Description |
| :--- | :--- | :--- |
| 🎨 `:art:` | `style` | Code formatting changes |
| ⚡️ `:zap:` | `perf` | Performance optimization |
| 🔥 `:fire:` | `remove` | Delete code or files |
| 🐛 `:bug:` | `fix` | Bug fix |
| 🚑️ `:ambulance:` | `hotfix` | Emergency fix |
| ✨ `:sparkles:` | `feat` | New feature |
| 📝 `:memo:` | `docs` | Documentation update |
| 🚀 `:rocket:` | `deploy` | Deployment related |
| 💄 `:lipstick:` | `ui` | UI/interface update |
| ✅ `:white_check_mark:` | `test` | Test related |
| 🔒 `:lock:` | `security` | Security fix |
| 🛡️ `:shield:` | `security` | Security feature |
| ♻️ `:recycle:` | `refactor` | Code refactoring |
| ➕ `:heavy_plus_sign:` | `add` | Add dependency |
| ➖ `:heavy_minus_sign:` | `remove` | Remove dependency |
| 📦 `:package:` | `build` | Build related |
| 🏗️ `:building_construction:` | `arch` | Architecture change |
| 🎯 `:dart:` | `target` | Target/quota related |
| 🧊 `:snowflake:` | `tlb` | TLB/cold-hot related |
| 🚀 `:rocket:` | `expansion` | Expansion/scaling |
| 🚨 `:rotating_light:` | `recovery` | Error recovery |
| 📊 `:bar_chart:` | `observability` | Observability |

### 5.2 Commit Format

```
<type>(<scope>): <emoji> <subject>

<body>

<footer>
```

**Example:**
```
feat(core): ⚡️ add Per-CPU lock-free freelist

- Implement per-CPU lock-free freelist for low-contention fast path
- Add mp_set_percpu_freelist() API
- Initialize freelist in mp_create() and destroy in mp_destroy()

Closes #123
```

### 5.3 Scope Convention

| Scope | Description |
| :--- | :--- |
| `core` | Core implementation (src/cmem.c) |
| `api` | Header files (include/*.h) |
| `test` | Test files |
| `bench` | Benchmark tests |
| `docs` | Documentation |
| `build` | Build system |
| `ci` | CI/CD |

---

## 6. Branch Strategy

### 6.1 Branch Naming

```
main          # Main branch, production-ready
develop       # Development branch
feature/*     # Feature branches
fix/*         # Fix branches
release/*     # Release branches
hotfix/*      # Emergency fix branches
```

### 6.2 Workflow

```bash
# 1. Create a feature branch from develop
git checkout -b feature/my-new-feature develop

# 2. Develop and commit
git commit -m "feat(core): ⚡️ add new feature"

# 3. Push to remote
git push origin feature/my-new-feature

# 4. Create a Pull Request
# 5. Merge into develop after Code Review passes
```

---

## 7. Documentation Writing Standards

### 7.1 Documentation Structure

Project documentation is located in the `docs/` directory:

```
docs/
├── architecture.md      # Architecture design document
├── api-reference.md     # API reference document
├── development.md       # Development guide (this document)
├── testing.md           # Testing guide
├── performance.md       # Performance tuning guide
├── security.md          # Security features document
└── troubleshooting.md   # Troubleshooting guide
```

### 7.2 Markdown Standards

- Use **GitHub Flavored Markdown**
- Heading levels should not exceed 3
- Code blocks must specify the language
- Table alignment uses `:---` syntax
- Links use relative paths

### 7.3 Documentation Update Checklist

When adding a new feature, make sure to update:

- [ ] `README.md`: Feature list and API index
- [ ] `docs/api-reference.md`: Detailed API description
- [ ] `docs/architecture.md`: Architecture changes (if any)
- [ ] `include/cmem.h`: File header comments
- [ ] `CHANGELOG.md`: Change log

---

## 8. Performance Tuning Tips

### 8.1 Pool Size Configuration

```c
// Choose appropriate initial capacity based on workload
size_t initial_capacity = 0;  // Auto-calculated
// Or specify manually
size_t initial_capacity = 64 * 1024 * 1024;  // 64MB
```

### 8.2 Flag Selection

| Scenario | Recommended Flag Combination |
| :--- | :--- |
| Production (high performance) | `MP_FLAG_THREAD_SAFE \| MP_FLAG_THREAD_LOCAL_CACHE` |
| Development/debugging | `MP_FLAG_THREAD_SAFE \| MP_FLAG_DEBUG_CANARY \| MP_FLAG_TRACK_LOCATIONS` |
| Security-sensitive | `MP_FLAG_THREAD_SAFE \| MP_FLAG_ENCRYPTED_MEMORY \| MP_FLAG_GUARD_PAGES` |
| High concurrency | `MP_FLAG_THREAD_SAFE \| MP_FLAG_PERCPU_FREELIST` |
| Large memory workload | `MP_FLAG_HUGE_PAGES \| MP_FLAG_HOT_COLD_SEPARATION` |

### 8.3 Auto-Compact Configuration

```c
mp_set_auto_compact(pool, true, 0.8, 0.3);
// Trigger compact when pressure > 80% or fragmentation > 30%
```

### 8.4 Per-CPU Freelist

```c
mp_set_percpu_freelist(pool, true);
// Applicable when:
// - CPU core count <= 64
// - Small object allocation is dominant
// - High concurrency scenario
```

---

## 9. Debugging Tips

### 9.1 Enabling Debug Features

```c
// Via code
mp_flags_t flags = MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS | MP_FLAG_POISON_ON_FREE;

// Via environment variable
setenv("CMEM_CONF", "canary=1,track=1,poison=1", 1);
```

### 9.2 Heap Auditing

```c
bool healthy = mp_audit_heap(pool);
if (!healthy) {
    fprintf(stderr, "Heap corruption detected!\n");
}
```

### 9.3 Leak Analysis

```c
char report[8192];
mp_analyze_leaks(pool, report, sizeof(report));
printf("%s\n", report);
```

### 9.4 Event Tracing

```c
mp_set_event_callback(pool, [](memory_pool_t* p, mp_event_type_t ev, void* ptr, size_t size, void* ud) {
    printf("Event: %d, ptr=%p, size=%zu\n", ev, ptr, size);
}, NULL);
```

### 9.5 GDB Debugging

```bash
# Launch GDB
gdb ./build/unit_tests

# Set breakpoint
break mp_alloc

# Run
run

# Inspect memory pool state
p *pool
```

---

## 10. FAQ

### Q1: How do I choose the right number of Slab Classes?

**A:** The default 7 Classes (8B~512B) are suitable for most scenarios. If the workload is concentrated in a specific size range, you can customize using `mp_set_slab_classes`.

### Q2: When should I enable Per-CPU Freelist?

**A:** It is recommended to enable when:
- CPU core count >= 4
- Small object allocation accounts for > 60%
- High multi-thread concurrency

### Q3: How do I diagnose memory fragmentation?

**A:**
```c
mp_stats_t stats;
mp_get_stats(pool, &stats);
printf("Fragmentation: %.2f%%\n", stats.fragmentation_ratio * 100);

// Check reclaimable memory
printf("Freeable: %zu bytes\n", mp_freeable(pool));
```

### Q4: What are the limitations of online expansion?

**A:**
- Only the TLSF layer supports online expansion
- The Slab layer and OS Fallback layer do not participate in expansion
- After expansion, the new capacity is only valid in the new TLSF Pool linked list

### Q5: What is the performance impact of encrypted memory?

**A:**
- `mlock` has system limits (typically 64KB~1GB per process, depending on system configuration)
- `MADV_DONTDUMP` has virtually no performance impact
- It is recommended to enable only in security-sensitive scenarios

### Q6: How do I integrate it into an existing project?

**A:**
```c
// Option 1: Directly replace malloc
#include "cmem_override.h"

// Option 2: Replace per module
memory_pool_t* pool = mp_create(1024*1024, MP_FLAG_DEFAULT);
void* p = mp_alloc(pool, size);
```

### Q7: What platforms are supported?

**A:**
- Linux (full support)
- macOS (partial support, no NUMA/HugePages)
- Windows (partial support, requires MSVC)
- FreeBSD/Android (basic support)

### Q8: How do I contribute code?

**A:**
1. Fork the repository
2. Create a feature branch
3. Write code + tests
4. Ensure `make test` passes
5. Submit a Pull Request

See [Contributing Guide](https://github.com/your-repo/cmem/blob/main/CONTRIBUTING.md) for details.