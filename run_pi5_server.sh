#!/bin/bash
set -euo pipefail

export PYENV_ROOT="$HOME/.pyenv"
export PATH="$PYENV_ROOT/bin:$PATH"
eval "$(pyenv init -)"

cd /home/pk/mICROOOOO
exec "$HOME/.pyenv/versions/3.11.9/bin/uvicorn" pi5_server:app --host 0.0.0.0 --port 8000
