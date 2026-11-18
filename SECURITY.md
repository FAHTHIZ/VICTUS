# Security Policy

VICTUS is early-stage; do not assume production security guarantees.

Report suspected vulnerabilities privately through GitHub private vulnerability reporting if enabled, or another private contact published by the owner. Do not publish exploit details for unpatched issues.

Include affected commit/platform, safe reproduction steps, expected/actual behavior, impact and whether project files/user data may be affected.

Sensitive areas: asset import/archive extraction; project/scene/save deserialization; scripts/plugins; network-facing features; paths/filesystem; build/cook pipelines and downloaded dependencies. Treat external input as untrusted. Validate sizes, paths, versions, references and decompression limits. Avoid executing imported content during inspection.

## Supported versions and response

Before 1.0.0 only the latest release (and `main`) receives security fixes. Maintainers acknowledge private reports within 3 working days and ship fixes as patch releases with a published advisory; the full procedure, including dependency advisories, is in [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md#security-response).

Crash reports and diagnostics bundles never leave your machine unless you send them; see [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) for what they contain before attaching one to a public issue.
