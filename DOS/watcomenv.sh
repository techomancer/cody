#!/bin/sh
# Source this file to set up the Open Watcom 1.9 build environment.
# Usage:  . ./env.sh   (from the DOS/ directory)

# Resolve the directory containing this script.
# Works with both "source ./env.sh" and ". /path/to/DOS/env.sh"
_env_self="${BASH_SOURCE[0]:-$0}"
SCRIPT_DIR="$(cd "$(dirname "$_env_self")" 2>/dev/null && pwd)"
# Fallback: if the above produced nothing (e.g. pure dot-sourcing), assume
# the caller is running from the DOS/ directory.
if [ -z "$SCRIPT_DIR" ] || [ "$SCRIPT_DIR" = "/" ]; then
    SCRIPT_DIR="$(pwd)"
fi

export WATCOM="$SCRIPT_DIR/WATCOM"
export PATH="$WATCOM/binl:$PATH"
export INCLUDE="$WATCOM/h"
export LIB="$WATCOM/lib286:$WATCOM/lib286/dos"

echo "Open Watcom 1.9 environment set:"
echo "  WATCOM  = $WATCOM"
echo "  PATH    = $PATH"
echo "  INCLUDE = $INCLUDE"
echo "  LIB     = $LIB"
