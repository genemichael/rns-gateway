"""
PlatformIO pre-build script: normalise paths in microStore's SPIFFS adapter.

ESP32's Arduino VFS rejects any path that does not start with "/" — it logs
  E vfs_api.cpp:29 open(): ./transport_identity does not start with /
and returns an invalid File. microReticulum builds its paths from
Reticulum::_storagepath, which is ".", giving "./cache", "./transport_identity"
and "./path_store". Those are fine on POSIX and rejected on-device, so the RNS
transport identity never persists and is regenerated on every boot.

Normalises "./x" and "x" -> "/x" at each SPIFFS.* call site in the adapter.
Idempotent, and FAILS THE BUILD rather than warning — a silently unapplied
patch produces a runtime failure weeks later, which is exactly what happened:
see the note on duplicate copies below.

*** Patch EVERY copy of the adapter, not just one. ***
microReticulum declares an unconstrained registry dependency on microStore, so
PlatformIO materialises a second copy alongside our pinned one:

    microStore/                                    <- our lib_deps pin
    microStore@src-<hash>/                         <- microReticulum's transitive dep

Both are on the include path. An earlier version of this script patched only
the first, the build linked the second, and the board silently regenerated its
transport identity on every boot with the adapter looking correctly patched on
disk. Globbing for all copies is the fix.

Ported from pyxis/patch_littlefs_paths.py, which does the same for LittleFS.

TODO(upstream): microStore's adapters should normalise this themselves.
Filed as a candidate against torlando-tech/microStore — until that lands, this
script is a parity liability and should be treated as one.
"""
Import("env")
import glob
import os
import sys

LIBDEPS = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))
PATTERN = os.path.join(LIBDEPS, "*", "include", "microStore", "Adapters",
                       "SPIFFSFileSystem.h")

MARKER = "_mc_norm_path"
ANCHOR = "namespace microStore { namespace Adapters {"

HELPER = """namespace microStore { namespace Adapters {

// patched: ESP32 Arduino VFS requires a leading "/". microReticulum uses
// "./"-prefixed paths that work on POSIX but are rejected here.
static inline std::string _mc_norm_path(const char* p) {
\tstd::string s(p ? p : "");
\tif (s.rfind("./", 0) == 0) s.erase(0, 2);
\tif (s.empty() || s.front() != '/') s.insert(s.begin(), '/');
\treturn s;
}
"""

REPLACEMENTS = [
    ("SPIFFS.open(path, pmode)",     "SPIFFS.open(_mc_norm_path(path).c_str(), pmode)"),
    ("SPIFFS.open(path, FILE_READ)", "SPIFFS.open(_mc_norm_path(path).c_str(), FILE_READ)"),
    ("SPIFFS.open(path)",            "SPIFFS.open(_mc_norm_path(path).c_str())"),
    ("SPIFFS.exists(path)",          "SPIFFS.exists(_mc_norm_path(path).c_str())"),
    ("SPIFFS.remove(path)",          "SPIFFS.remove(_mc_norm_path(path).c_str())"),
    ("SPIFFS.rename(from_path, to_path)",
     "SPIFFS.rename(_mc_norm_path(from_path).c_str(), _mc_norm_path(to_path).c_str())"),
    ("SPIFFS.mkdir(path)",           "SPIFFS.mkdir(_mc_norm_path(path).c_str())"),
    ("SPIFFS.rmdir(path)",           "SPIFFS.rmdir(_mc_norm_path(path).c_str())"),
]

# Every path-taking SPIFFS call site must end up normalised. If the adapter
# gains one upstream, this count changes and the assertion below catches it
# rather than letting an unnormalised path through unnoticed.
MIN_CALL_SITES = len(REPLACEMENTS)


def fail(message):
    print("PATCH ERROR: " + message, file=sys.stderr)
    env.Exit(1)


def patch_one(path):
    with open(path, "r") as f:
        content = f.read()

    label = path.replace(LIBDEPS + os.sep, "")

    if MARKER in content:
        print("PATCH: %s (already applied)" % label)
        return

    if ANCHOR not in content:
        fail("%s: namespace anchor not found. microStore's adapter layout "
             "changed — re-derive this patch against the new pin." % label)
        return

    content = content.replace(ANCHOR, HELPER.rstrip("\n"), 1)

    applied = 0
    for old, new in REPLACEMENTS:
        if old in content:
            content = content.replace(old, new)
            applied += 1

    if applied < MIN_CALL_SITES:
        fail("%s: only %d of %d SPIFFS call sites matched. The adapter's "
             "signatures changed; an unnormalised path would reach the VFS "
             "and the transport identity would silently stop persisting."
             % (label, applied, MIN_CALL_SITES))
        return

    if "#include <string>" not in content:
        content = content.replace("#include <SPIFFS.h>",
                                  "#include <SPIFFS.h>\n#include <string>", 1)

    with open(path, "w") as f:
        f.write(content)
    print("PATCH: %s (%d call sites)" % (label, applied))


targets = sorted(glob.glob(PATTERN))

if not targets:
    fail("no copy of microStore/Adapters/SPIFFSFileSystem.h found under %s. "
         "Without it the RNS transport identity will not persist." % LIBDEPS)
else:
    print("PATCH: normalising SPIFFS paths in %d microStore cop%s"
          % (len(targets), "y" if len(targets) == 1 else "ies"))
    for target in targets:
        patch_one(target)
