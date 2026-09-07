#!/bin/bash
# Run the hub in the foreground. Uses the repo venv if there is one; the hub
# itself is standard library only, so system python works too.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py="$here/../.venv/bin/python"
[ -x "$py" ] || py="$(command -v python3)"
cd "$here"
exec "$py" -m beacon hub "$@"
