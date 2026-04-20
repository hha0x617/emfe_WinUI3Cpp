# Security Policy

## Supported Versions

Only the latest release receives security updates during the pre-1.0 phase.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | ✅        |
| < 0.1   | ❌        |

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Report privately via GitHub's **Private Vulnerability Reporting**:

1. Go to the **Security** tab of this repository.
2. Click **Report a vulnerability**.

Alternatively, email: <hha0x617@users.noreply.github.com>

Please include:

- Affected version / commit hash
- Steps to reproduce
- Impact assessment
- Proof-of-concept if available

## Response

- Initial response: within 7 days
- Fix timeline depends on severity and complexity
- A GitHub Security Advisory will be published after the fix is released
- Reporters will be credited unless they request anonymity

## Out of Scope

- Vulnerabilities in guest operating systems (NetBSD, Linux) running inside
  the emulator — report those to the upstream project
- Vulnerabilities inside plugin DLLs (`emfe_plugin_*.dll`) loaded at
  runtime — report those to the
  [emfe_plugins](https://github.com/hha0x617/emfe_plugins) repository
- Issues requiring physical access to the host machine
- Denial of service caused by crafted ROM, disk, or configuration files —
  the emulator is not a sandbox and is not designed to execute untrusted
  guest software safely
- Vulnerabilities in third-party dependencies (Windows App SDK, WinUI 3,
  C++/WinRT, etc.) unless directly exploitable through this project's code
