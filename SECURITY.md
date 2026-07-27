# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |

## Reporting a Vulnerability

If you discover a security vulnerability in cmem, please report it responsibly.

**Do not** open a public GitHub issue for security vulnerabilities.

### Reporting Process

1. Email security reports to: **cmem@example.com**
2. Include the following information:
   - Project and component affected
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact assessment
   - Suggested fix (if available)

### Response Timeline

- **Acknowledgment**: Within 48 hours
- **Initial Assessment**: Within 7 days
- **Fix Timeline**: Depends on severity
  - Critical: 7-14 days
  - High: 14-30 days
  - Medium: 30-60 days
  - Low: Next scheduled release

### Disclosure Policy

- Security fixes are released as patch versions
- Credit is given to reporters (unless anonymity is requested)
- Details are disclosed after a fix is available

## Security Best Practices for Users

- Enable `MP_FLAG_DEBUG_CANARY` in debug builds
- Enable `MP_FLAG_POISON_ON_FREE` in debug builds
- Use `MP_FLAG_THREAD_SAFE` in multi-threaded environments
- Run with AddressSanitizer (`-fsanitize=address`) during testing
- Set memory limits with `mp_set_memory_limit` to prevent unbounded growth
- Use `mp_set_watermark_callback` for proactive pressure monitoring
