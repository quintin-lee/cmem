# cmem Documentation

Welcome to the cmem project documentation. This documentation center covers architecture design, API reference, development guide, testing, performance tuning, and security features.

## 📚 Documentation Index

### Core Documentation

| Document | Description | Audience |
| :--- | :--- | :--- |
| [architecture.md](architecture.md) | Architecture design document | Architects, senior developers |
| [api-reference.md](api-reference.md) | API reference document | All developers |
| [development.md](development.md) | Development and contribution guide | Contributors, developers |
| [testing.md](testing.md) | Testing guide | QA, developers |
| [performance.md](performance.md) | Performance tuning guide | Performance engineers, developers |
| [security.md](security.md) | Security features document | Security engineers, developers |

### Quick Start

1. **New Users**: Start with [README.md](../../README.md) for project overview
2. **Integration**: Check [api-reference.md](api-reference.md) for API details
3. **Performance**: Refer to [performance.md](performance.md) for tuning
4. **Security**: See [security.md](security.md) for security hardening
5. **Contributing**: Read [development.md](development.md) for workflow

---

## 📖 Document Overview

### architecture.md

Covers:
- System overview and design goals
- Three-tier hybrid architecture (Slab / TLSF / OS Fallback)
- Core data structures and memory layout
- Concurrency control model and lock strategy
- Extension mechanisms (online expansion, custom backends, event callbacks)
- Diagnostics and observability design
- Security feature architecture
- C++ interface design
- Performance optimization strategies
- Future evolution directions

### api-reference.md

Covers:
- Memory pool lifecycle APIs (create/destroy/reset)
- Memory allocation and deallocation APIs (alloc/calloc/realloc/free)
- Introspection and metadata APIs (usable_size/ptr_valid)
- Convenience helpers (strdup/memdup/asprintf)
- Memory compaction and reclamation APIs (trim/compact/purge)
- Statistics, diagnostics, and monitoring APIs (stats/histogram/html/prometheus)
- Leak detection and heap audit APIs (audit/analyze/check)
- Advanced feature APIs (hot-reload, auto-compaction, latency stats, Slab config)
- Configuration flags (mp_flags_t)
- C++ APIs (MemoryPool/allocator/pmr_resource)
- Event type enumeration

### development.md

Covers:
- Development environment setup (Linux/macOS/Windows)
- Code standards (C11/C++17)
- Build systems (Makefile/CMake)
- Testing guide (adding tests, running tests, coverage)
- Commit conventions (Gitmoji)
- Branching strategy
- Documentation standards
- Performance tuning suggestions
- Debugging techniques (GDB/ASan/Valgrind)
- FAQ

### testing.md

Covers:
- Test framework overview
- Test structure and directories
- Running tests (C/C++/Benchmark)
- Adding new tests (template code)
- Test coverage checklist (35+ test cases)
- Sanitizers usage (ASan/UBSan)
- Performance benchmarks
- CI/CD configuration
- Debugging checklist

### performance.md

Covers:
- Performance model (three-tier allocator throughput comparison)
- Pool size tuning (initial capacity/online expansion/limits)
- Flag selection strategy (overhead comparison table)
- Slab class tuning (custom size tables/monitoring distribution)
- Concurrency optimization (TLS Cache vs Per-CPU Freelist/batch operations/Frame Arena)
- Memory reclamation strategies (auto-compaction/manual reclamation/lazy reclamation)
- NUMA optimization (node binding/multi-node scenarios)
- HugePages optimization (system configuration/use cases)
- Monitoring metrics (Prometheus/JSON)
- Performance pitfalls and troubleshooting

### security.md

Covers:
- Security threat model (threat classification/attack surface)
- Debugging protections (Canary/UAF poisoning/Guard Pages/double free)
- Encrypted memory (mlock/MADV_DONTDUMP/secure zero)
- ASan integration (detection capabilities/integration mode/custom error reporting)
- Security configuration examples (maximum/balanced/minimum security)
- Security best practices (development/production/event callbacks)
- Compliance (PCI DSS/GDPR/HIPAA/SOC 2)

---

## 🔒 ABI Stability & Versioning

### ABI Version

The current ABI version is `1`. Use `mp_abi_version()` to query at runtime.

### Stability Promise

| Change Type | ABI Break? | Semantic Version |
| :--- | :--- | :--- |
| Bug fix (no API change) | No | Patch |
| New API (backward compatible) | No | Minor |
| New API (backward incompatible) | Yes | Major |
| Internal refactoring | No | Patch |
| Struct layout change | Yes | Major |
| Flag enum value change | Yes | Major |

### Compatibility Rules

- New fields are appended only at the end of public structs.
- `mp_flags_t` enum values are append-only; do not reuse or renumber existing values.
- Old clients can detect newer pool versions via `mp_abi_version()` and degrade gracefully.

---

## 🖥️ Platform Support

| Platform | Status | Notes |
| :--- | :--- | :--- |
| **Linux (glibc)** | ✅ Full | NUMA, HugePages, Shared Memory, madvise, mlock |
| **Linux (musl)** | ⚠️ Partial | Basic allocator works; NUMA/HugePages may need porting |
| **macOS** | ⚠️ Partial | No NUMA/HugePages/Shared Memory; madvise limited |
| **Windows (MSVC)** | ⚠️ Partial | Requires porting mmap/madvise to VirtualAlloc |
| **FreeBSD** | ⚠️ Basic | May work with minor #ifdef adjustments |
| **Android** | ⚠️ Basic | Bionic libc; test before production |

### Compiler Support

| Compiler | Minimum | Recommended |
| :--- | :--- | :--- |
| GCC | 7.0 | 13.0+ |
| Clang | 6.0 | 17.0+ |
| MSVC | 2017 | 2022+ |

---

## 🔗 External Resources

- [GitHub Repository](https://github.com/your-repo/cmem)
- [Issue Tracker](https://github.com/your-repo/cmem/issues)
- [Discussions](https://github.com/your-repo/cmem/discussions)
- [Doxygen Documentation](https://your-repo.github.io/cmem/)

---

## 📝 Contributing to Documentation

Found an issue or want to add content?

1. Fork the repository
2. Modify the corresponding file in `docs/` directory
3. Submit a Pull Request

**Documentation Standards:**
- Use Markdown format
- Specify language for code blocks
- Align tables using `:---` syntax
- Use relative paths for links
