# Security Policy

## Supported Versions

Only the latest commit on the `main` branch is actively supported with security patches and bug fixes.

| Component | Supported | Notes |
|---|---|---|
| `SiegeLauncher` | :white_check_mark: | Auto-updated via update manifest |
| `SiegeHost` (Mss32.dll) | :white_check_mark: | Auto-updated via update manifest |
| `SiegePortal` | :white_check_mark: | Main branch |
| `SiegeServer` | :white_check_mark: | Main branch |
| `Web Portal` | :white_check_mark: | Main branch |

## Reporting a Vulnerability or Exploit

If you discover a security vulnerability, server crash vector, or game-breaking multiplayer exploit (such as item duplication or arbitrary state manipulation):

1. **Do NOT open a public issue.** Public disclosure exposes active community servers and players to disruption before a patch can be deployed.
2. **Submit a Private Vulnerability Report:**
   - Use GitHub's **Private Vulnerability Reporting** feature on this repository (under the **Security** tab -> **Report a vulnerability**).
   - Alternatively, reach out directly to the maintainer on Discord or email.
3. **Include details:**
   - A description of the issue or exploit mechanism.
   - Clear steps or packet captures to reproduce the behavior in a controlled environment.
   - Any potential remediation ideas or suggested patches.

We appreciate responsible disclosure and will investigate and patch confirmed vulnerabilities as quickly as possible.
