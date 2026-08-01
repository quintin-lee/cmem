# Contributing to cmem

Thank you for your interest in contributing to cmem! This document explains the development workflow, coding standards, and commit conventions.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Build & Test](#build--test)
- [Coding Standards](#coding-standards)
- [Commit Conventions](#commit-conventions)
- [Pull Request Process](#pull-request-process)
- [Reporting Issues](#reporting-issues)

## Code of Conduct

Be respectful, constructive, and collaborative. We follow the [Contributor Covenant](https://www.contributor-covenant.org/) spirit.

## Getting Started

1. Fork the repository
2. Clone your fork:
   ```bash
   git clone https://github.com/<your-username>/cmem.git
   cd cmem
   ```
3. Add the upstream remote:
   ```bash
   git remote add upstream https://github.com/quintin-lee/cmem.git
   ```

## Development Workflow

1. Sync with upstream:
   ```bash
   git checkout master
   git pull upstream master
   ```
2. Create a feature branch:
   ```bash
   git checkout -b feat/my-new-feature
   ```
3. Make changes and ensure all tests pass locally.
4. Format your code:
   ```bash
   make format
   ```
5. Run tests:
   ```bash
   make test
   make test_advanced
   ```
6. Commit using the [gitmoji convention](#commit-conventions).
7. Push to your fork and open a Pull Request against `master`.

## Build & Test

### Prerequisites

- GCC or Clang with C11 / C++17 support
- Make or CMake 3.10+
- pthread, librt (Linux)

### Makefile (quick start)

```bash
make test          # build + unit tests
make test_advanced # advanced unit tests
make test_cpp      # C++ STL / PMR tests
make test-fast     # skip format check
make fuzz          # run libFuzzer harness locally (60s)
```

### CMake (alternative)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Sanitizers

```bash
make clean
CC=clang CFLAGS="-fsanitize=address,undefined -g -O1" make test
```

## Coding Standards

- Language: C11 / C++17
- Style: enforced by [clang-format](.clang-format)
  ```bash
  make format   # auto-format
  make format-check  # CI-style dry run
  ```
- Static analysis:
  ```bash
  make static-analysis
  ```
- No warnings under `-Wall -Wextra -Werror` in CI builds.

## Commit Conventions

We follow the [gitmoji](https://gitmoji.dev/) convention. Each commit message must follow this format:

```
[type]([scope]): [emoji] [subject]
```

### Rules

- **Emoji MUST come after the colon**: `fix(core): 🐛 ...` — NOT `🐛 fix(core): ...`
- **ONE emoji per commit**: Never double emojis like `✨🎨 feat(...)`
- **Lowercase subject**: Start with lowercase letter
- **No trailing period**: `fix(core): 🐛 add timeout` — NOT `fix(core): 🐛 add timeout.`
- **Imperative mood**: "add" not "added", "fix" not "fixed"

### Types & Emojis

| Type       | Emoji | When to Use                                     |
|------------|-------|--------------------------------------------------|
| `feat`     | ✨     | New feature or functionality                     |
| `fix`      | 🐛     | Bug fix                                          |
| `docs`     | 📝     | Documentation only                              |
| `style`    | 🎨     | Code formatting, whitespace, semicolons          |
| `refactor` | ♻️     | Code restructuring without behavior change       |
| `perf`     | ⚡️    | Performance improvement                          |
| `test`     | ✅     | Adding or fixing tests                           |
| `build`    | 📦     | Build system or dependency changes               |
| `ci`       | 👷     | CI/CD configuration changes                      |
| `chore`    | 🧹     | Tooling, config, non-src/test changes            |
| `revert`   | ⏪️    | Reverting a previous commit                      |

### Examples

```
feat(core): ✨ add thread-local cache optimization
fix(ci): 🐛 fix clang-format path on macOS
docs(readme): 📝 update installation instructions
refactor(slab): ♻️ extract page creation helper
style(fuzz): 🎨 align braces in switch cases
perf(tlsf): ⚡️ optimize block coalescing
test(asan): ✅ add AddressSanitizer integration tests
build(cmake): 📦 add fuzz_alloc target
ci(github): 👷 add macOS runner job
chore(deps): 🧹 update submodule references
revert: ⏪️ revert "feat: ✨ experimental ring buffer"
```

## Pull Request Process

1. Ensure the PR title follows the [commit convention](#commit-conventions).
2. All CI checks must pass (Linux, macOS, Windows, ASan, TSan, Fuzzing, Codecov).
3. At least one maintainer review is required for non-trivial changes.
4. Update `CHANGELOG.md` under `[Unreleased]` if your change is user-facing.

## Reporting Issues

- **Bugs**: Open a GitHub Issue with reproduction steps, environment info, and logs.
- **Security**: See [`SECURITY.md`](SECURITY.md) — do NOT open a public issue.
- **Features**: Open a GitHub Issue describing the use case and proposed API.
