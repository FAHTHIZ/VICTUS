# Security Policy

VICTUS is early-stage; do not assume production security guarantees.

Report suspected vulnerabilities privately through GitHub private vulnerability reporting if enabled, or another private contact published by the owner. Do not publish exploit details for unpatched issues.

Include affected commit/platform, safe reproduction steps, expected/actual behavior, impact and whether project files/user data may be affected.

Sensitive areas: asset import/archive extraction; project/scene/save deserialization; scripts/plugins; network-facing features; paths/filesystem; build/cook pipelines and downloaded dependencies. Treat external input as untrusted. Validate sizes, paths, versions, references and decompression limits. Avoid executing imported content during inspection.
