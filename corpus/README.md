# Fuzzing Corpus

Seed inputs for libFuzzer harness (`tests/fuzz_alloc.c`).

## 使用
```bash
# Interactive fuzzing
make fuzz-run
# CI mode (quick, non-interactive)
make fuzz-ci
# Clean build artifacts
make fuzz-clean
```

## Seed 说明
初始 seed 文件应包含合法的 fuzzing 输入（至少 2 字节：op + arg）。
crash-* 和 timeout-* 由 libFuzzer 自动保存，用于回归测试。
