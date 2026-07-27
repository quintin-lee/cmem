---
name: Bug Report
about: Create a report to help us improve cmem
title: '[BUG] '
labels: bug
assignees: ''
---

## Describe the Bug

A clear and concise description of what the bug is.

## To Reproduce

Steps to reproduce the behavior:

1. Create pool with flags '...'
2. Call `mp_alloc(...)` with size '...'
3. Observe behavior '...'

## Expected Behavior

A clear and concise description of what you expected to happen.

## Actual Behavior

What actually happened.

## Environment

- cmem version: [e.g. 1.0.0]
- Compiler: [e.g. gcc 11.2.0]
- Platform: [e.g. Ubuntu 22.04, Linux 5.15]
- Build type: [e.g. Debug/Release]
- Sanitizer: [e.g. ASan enabled]

## Additional Context

Add any other context about the problem here.

## Checklist

- [ ] I have searched existing issues
- [ ] I have reproduced this with the latest version
- [ ] I have enabled `MP_FLAG_DEBUG_CANARY` if applicable
- [ ] I have run with AddressSanitizer if applicable
