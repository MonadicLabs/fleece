#!/bin/sh
# Applies every patches/*.patch to the FetchContent'd rns_microreticulum
# source, run from that source dir as CMake's PATCH_COMMAND (see fleece's
# own CMakeLists.txt FetchContent_Declare(rns_microreticulum ...) for why).
#
# Idempotent: FetchContent can re-run this against an already-populated
# (and thus already-patched) source on a reconfigure, and a plain
# `git apply` errors out the second time since the lines it's matching are
# already changed. `--check` first (does this still apply cleanly, i.e.
# NOT yet applied?) gates each real apply.
#
# A real script file rather than an inline PATCH_COMMAND one-liner: CMake
# treats unescaped `;` in a bare argument as its own list separator, which
# silently mangles any inline shell script containing `;`/`&&` chains
# (confirmed live -- the inline version's `for ... ; do ... ; done` got
# split into garbage argv). A script file sidesteps that entirely.
set -e
here="$(dirname "$0")"
for p in "$here"/*.patch; do
    if git apply --check "$p" 2>/dev/null; then
        git apply "$p"
    fi
done
