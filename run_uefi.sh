#!/usr/bin/env bash
# run_uefi.sh — Quick UEFI launcher.  Delegates to tools/smoke.sh.
# Called by: make run-uefi
exec "$(dirname "$0")/tools/smoke.sh" --uefi "$@"
