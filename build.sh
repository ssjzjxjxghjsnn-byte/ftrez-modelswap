#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ $# -eq 0 ]]; then
    set -- :app:assembleDebug :app:lintDebug
fi
exec ./gradlew "$@"
