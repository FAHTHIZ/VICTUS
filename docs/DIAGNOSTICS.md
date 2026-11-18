# Crash reports and diagnostics

VICTUS keeps crash reports and session logs on your machine and can bundle them into one zip for a bug report. Nothing is uploaded, ever; you decide what to send and to whom. Design: ADR-0072 in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

## Where files go

The diagnostics folder is `VICTUS_DIAGNOSTICS_DIR` if set, otherwise `diagnostics/` inside the per-user VICTUS data folder (`$XDG_CONFIG_HOME/victus` or `~/.config/victus` on Linux, `%APPDATA%\VICTUS` on Windows, `~/Library/Application Support/VICTUS` on macOS).

| Folder | Contents | Kept |
|---|---|---|
| `crashes/` | `crash-<unix time>-<pid>-<app>.txt`, one per crash | newest 20 |
| `logs/` | `<app>-<unix time>-<pid>.log`, one per session (`victus run`/`serve`/`simulate`, the player) | newest 5 per app |
| `sessions/` | `<app>-<pid>.running` while a session runs | removed on clean exit |

A `.running` marker whose process no longer exists means that session crashed, was killed or lost power; the next start of the same app logs "the previous ... session ended unexpectedly".

## What a crash report contains

```
VICTUS crash report
format: 1
reason: SIGSEGV (invalid memory access)
fault_address: 0x0
app: victus_player
pid: 31174
unix_time: 1795007111
uptime_seconds: 42
version: 0.2.0
revision: d7a0232c2e30
platform: linux x86_64
compiler: gcc 15.2.0
build: Release
--- backtrace ---
#3 0x561057937271 /opt/game/victus_player+0x162a271 [build-id c3bb1dfa...]
...
--- recent log ---
(the last 64 log lines)
```

Reasons are fatal signals (SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS), Windows structured exceptions, `std::terminate` with the exception message, and failed `VICTUS_VERIFY`/`VICTUS_ASSERT` (shown as SIGABRT, with the assertion text in the recent log). Tools can call `crash::write_report(reason)` to record a non-fatal problem in the same format.

The handler runs inside the crashing process. It is written to avoid allocation and locks, but a corrupted heap or stack can still stop it. There are no minidumps.

## Diagnostics bundles

```
victus diagnose [project] --out bug.zip
victus diagnose --list
```

| Bundle file | Option to leave it out |
|---|---|
| `system.txt`: engine version, revision, build type, compiler, OS name/release/version, CPU threads, RAM | `--no-system` |
| `crashes/*.txt`: the newest 10 reports | `--no-crashes` |
| `logs/*.log`: the newest 3 session logs (last 4 MiB of each) | `--no-logs` |
| `project/victus.project` (settings only) | `--no-project` |
| `project/validation.txt`, `validation.json`: `victus check` results | `--no-validation` |
| `README.txt`, `manifest.json`: what is inside and how it was made | always |

Privacy defaults:

- The home folder, the project folder and the user name are replaced by `<home>`, `<project>` and `<user>` in every file (`--keep-paths` turns this off). The README states how many replacements were made.
- The host name, hardware serial numbers, network addresses and environment variables are never collected.
- Scenes, scripts, textures and other assets are never included.
- Open the zip and read it before attaching it anywhere.

Code (the editor, tools) creates the same bundle with `player::create_diagnostics_bundle` in `victus/player/diagnostics.hpp` (target `VICTUS::Player`); `BundleOptions::extra` adds caller-specific files such as a GPU report under `extra/`.

## Turning stack frames into function names

Frames are module path + offset. With the matching debug information:

```
python3 tools/crash/symbolize.py crash-....txt [--symbols DIR ...]
```

It uses `llvm-symbolizer` or `addr2line`, looks for `DIR/.build-id/aa/rest.debug`, `DIR/<module>.debug`, `DIR/<module>` and the module path from the report, and only uses a file whose GNU build-id matches the report, so a rebuilt binary never gives wrong names. Release builds keep debug information in a separate symbol archive (release process: docs/RELEASE_PROCESS.md, in progress); keep it for every build you ship.

Windows frames have no build-id; resolve them with the matching `.pdb` (WinDbg, or `llvm-symbolizer --obj=game.exe`). Android reports have no frames.

## Testing the handlers

`victus crash-test segv|abort|terminate|assert` crashes on purpose; `ctest -R victus_cli_diagnostics` does this in a temporary diagnostics folder and checks the reports and the bundle.
