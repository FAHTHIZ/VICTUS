# VICTUS Script Debugger for VS Code

Debugs a project's Luau scripts through `victus debug-adapter` (Debug Adapter Protocol over stdio,
ADR-0099). The game runs headless (the same simulation as `victus simulate`).

Install for development: copy or symlink this folder into `~/.vscode/extensions/victus-debug`,
or open it in VS Code and press F5. Set `victus.executable` if `victus` is not on PATH.

`launch.json`:

```json
{ "type": "victus", "request": "launch", "name": "Debug VICTUS scripts",
  "project": "${workspaceFolder}", "frames": 0, "stopOnEntry": false }
```

Supported: line, conditional and hit-count breakpoints, logpoints, stop on uncaught script errors,
step over/in/out, pause, call stack, locals/upvalues/table expansion, watch and hover evaluation,
editing locals. Other editors with DAP support (Neovim nvim-dap, Zed, Helix) can run
`victus debug-adapter` the same way.
