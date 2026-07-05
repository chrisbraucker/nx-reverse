#!/bin/bash
# -+- coding: UTF-8 -+-

set -eu

WORKSPACE_FOLDER="${WORKSPACE_FOLDER:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [ -d "/codex" ] && [ ! -e "$HOME/.codex" ]; then
  ln -sT /codex "$HOME/.codex"
fi

bash "${WORKSPACE_FOLDER}/.devcontainer/toolchain/init-workspace.sh"

HISTFILE=/history/.bash_history
touch "$HISTFILE"

SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=$HISTFILE"
if ! grep -Fq "$SNIPPET" "$HOME/.bashrc" 2>/dev/null; then
  echo "$SNIPPET" >> "$HOME/.bashrc"
fi
