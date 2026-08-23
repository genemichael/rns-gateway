#!/usr/bin/env bash
#
# Parity gate for the additive-only MeshCore fork.
#
# The whole value of this fork's structure is that upgrading MeshCore is a
# `git rebase` onto the new tag with, at worst, one conflict in the variant ini.
# That property holds only while the diff against upstream is purely additive.
# Parity discipline that isn't enforced decays, so this fails the build the
# moment someone edits or deletes an upstream file.
#
# The rules:
#   1. No upstream file may be deleted or renamed.
#   2. Exactly one pre-existing upstream file may be touched —
#      variants/heltec_v4/platformio.ini — and only by appending (0 deletions).
#   3. Everything else in the diff must be a brand-new file.
#
# Usage:  scripts/check_parity.sh [upstream-ref]
#         UPSTREAM_REF=v1.17.1 scripts/check_parity.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# The one upstream file we are allowed to modify, and only additively.
APPEND_ONLY_FILE="variants/heltec_v4/platformio.ini"

# Prose exception: the standalone release repo (genemichael/rns-gateway) shows
# the root README as the product's front page, so it is REPLACED, not appended.
# README.md is documentation, not code — replacing it cannot conflict with an
# upstream rebase in any way that matters (take ours, always). Code files never
# get this exemption.
REPLACE_OK_FILE="README.md"

resolve_ref() {
  if [ -n "${1:-}" ]; then echo "$1"; return; fi
  if [ -n "${UPSTREAM_REF:-}" ]; then echo "$UPSTREAM_REF"; return; fi
  for candidate in upstream/main origin/main; do
    if git rev-parse --verify --quiet "$candidate" >/dev/null; then
      echo "$candidate"; return
    fi
  done
  echo "ERROR: no upstream ref found. Add an 'upstream' remote, or pass one:" >&2
  echo "       scripts/check_parity.sh v1.17.1" >&2
  exit 2
}

REF="$(resolve_ref "${1:-}")"
echo "Parity check against ${REF} ($(git rev-parse --short "$REF"))"
echo

fail=0

# ── Rule 1: nothing upstream may be deleted or renamed ──────────────────────
removed="$(git diff --diff-filter=DR --name-only "$REF" -- . || true)"
if [ -n "$removed" ]; then
  echo "FAIL: upstream files deleted or renamed —"
  echo "$removed" | sed 's/^/  /'
  echo "  A fork that removes upstream code conflicts on every rebase, forever."
  echo "  Our role should not USE these files, not delete them."
  echo
  fail=1
fi

# ── Rules 2 and 3: every changed path is either brand new, or the one ───────
# ── append-only file with zero deletions.                             ───────
while IFS=$'\t' read -r added deleted path; do
  [ -z "${path:-}" ] && continue
  # Binary files report '-' for both counts; treat them as 0 for the arithmetic
  # and let the "is it new?" test below decide.
  [ "$added" = "-" ] && added=0
  [ "$deleted" = "-" ] && deleted=0

  if ! git cat-file -e "${REF}:${path}" 2>/dev/null; then
    continue          # brand-new file — always fine, this is an additive fork
  fi

  if [ "$path" = "$REPLACE_OK_FILE" ]; then
    continue          # documentation front page — replacement is intentional
  fi

  if [ "$path" = "$APPEND_ONLY_FILE" ]; then
    if [ "$deleted" -ne 0 ]; then
      echo "FAIL: ${path} has ${deleted} deleted line(s)."
      echo "  This file may only be APPENDED to — one clearly delimited block"
      echo "  containing the base section and the [env:]. Rewriting upstream"
      echo "  lines here is what turns a clean rebase into a real conflict."
      echo
      fail=1
    fi
    continue
  fi

  echo "FAIL: upstream file modified: ${path}  (+${added} -${deleted})"
  echo "  Only ${APPEND_ONLY_FILE} may be touched. Put new code in"
  echo "  examples/rns_gateway/, lib/, test/host/ or scripts/ instead."
  echo
  fail=1
done < <(git diff --numstat "$REF" -- .)

if [ "$fail" -ne 0 ]; then
  echo "Parity gate FAILED."
  exit 1
fi

echo "Parity gate passed — diff against ${REF} is additive-only."
echo
git diff --stat "$REF" -- . | tail -5
