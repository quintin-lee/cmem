# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |

## Reporting a Vulnerability

If you discover a security vulnerability in cmem, please report it responsibly.

**Do not** open a public GitHub issue for security vulnerabilities.

### Reporting Channels

You may report vulnerabilities through either of the following channels:

1. **GitHub Private Vulnerability Reporting** (preferred)
   - Use the "Security" tab on this repository and click "Private vulnerability reporting"
   - This creates a private advisory visible only to maintainers

2. **Email**
   - Send details to: **cmem@example.com**
   - Include the information listed below

### Required Information

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

### Maintainer Note: Enabling Private Vulnerability Reporting

Maintainers should enable GitHub Private Vulnerability Reporting:
1. Go to repository **Settings → Security → Private vulnerability reporting**
2. Click **Enable private vulnerability reporting**
3. Configure the default recipient (e.g., the maintainer email above)

This allows reporters to submit security advisories directly through GitHub without exposing details publicly.

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
