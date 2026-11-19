// SPDX-License-Identifier: Apache-2.0
// Starts `victus debug-adapter` (ADR-0099) for "victus" debug sessions.
const vscode = require("vscode");

function activate(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory("victus", {
            createDebugAdapterDescriptor() {
                const exe = vscode.workspace.getConfiguration("victus").get("executable") || "victus";
                return new vscode.DebugAdapterExecutable(exe, ["debug-adapter"]);
            },
        }),
        vscode.debug.registerDebugConfigurationProvider("victus", {
            resolveDebugConfiguration(folder, config) {
                if (!config.type && !config.request && !config.name) {
                    config.type = "victus";
                    config.request = "launch";
                    config.name = "Debug VICTUS scripts";
                }
                if (!config.project && folder) config.project = folder.uri.fsPath;
                return config;
            },
        })
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
