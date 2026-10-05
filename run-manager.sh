#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
python=${GMUX_MANAGER_PYTHON:-"$(dirname "$project_dir")/tools/gmux-manager-venv/bin/python"}
if [[ ! -x "$python" ]]; then
  echo 'Install manager/requirements.txt in a virtual environment and set GMUX_MANAGER_PYTHON to its Python executable.' >&2
  exit 1
fi
cd "$project_dir"
exec "$python" -m manager.app "$@"
