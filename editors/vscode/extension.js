'use strict';

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');

function resolveMoExecutable() {
  const configured = vscode.workspace.getConfiguration('mo').get('executablePath', 'mo');
  if (configured && configured !== 'mo') {
    return configured;
  }
  const localBin = path.join(os.homedir(), '.local', 'bin', 'mo');
  try {
    fs.accessSync(localBin, fs.constants.X_OK);
    return localBin;
  } catch (_) {
    return 'mo';
  }
}

function formatBuffer(text, token) {
  return new Promise((resolve) => {
    const exe = resolveMoExecutable();
    const child = cp.spawn(exe, ['format', '--stdin'], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    const outChunks = [];
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        child.kill('SIGKILL');
        resolve(null);
      }
    }, 10000);

    if (token) {
      token.onCancellationRequested(() => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          child.kill('SIGTERM');
          resolve(null);
        }
      });
    }

    child.stdout.on('data', (chunk) => outChunks.push(chunk));
    child.on('error', () => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        resolve(null);
      }
    });
    child.on('close', (code) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        if (code === 0) {
          resolve(Buffer.concat(outChunks).toString('utf8'));
        } else {
          resolve(null);
        }
      }
    });

    child.stdin.on('error', () => {});
    child.stdin.end(text, 'utf8');
  });
}

function activate(context) {
  const provider = vscode.languages.registerDocumentFormattingEditProvider('mo', {
    async provideDocumentFormattingEdits(document, _options, token) {
      const original = document.getText();
      const formatted = await formatBuffer(original, token);
      if (formatted === null || formatted === original) {
        return [];
      }
      const lastLine = document.lineCount - 1;
      const fullRange = new vscode.Range(
        new vscode.Position(0, 0),
        document.lineAt(lastLine).range.end
      );
      return [vscode.TextEdit.replace(fullRange, formatted)];
    },
  });
  context.subscriptions.push(provider);
}

function deactivate() {}

module.exports = { activate, deactivate };
