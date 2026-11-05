# Source this for the project's toolchain.
EMSDK_QUIET=1 source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/emsdk/emsdk_env.sh" >/dev/null 2>&1
command -v emcc >/dev/null || echo "emcc not found: expected ../emsdk next to the repo" >&2
