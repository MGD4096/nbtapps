# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""The head units a build is for, and what the check and package tools hold its programs to.

NBT_PLATFORM in the environment picks one; the Makefile sets it from PLATFORM=. With none it is nbtevo, so every
tool does what it did before EntryEvo was a target.

  nbtevo     NBT Evo: QNX Neutrino 6.5.0 SP1, ARMv7 soft-float calls. release/NbtApp, release/agent, release/pkg
  entryevo   EntryEvo (EntryNav2): Linux 3.12, glibc 2.15, ARMv7 hard-float calls (as the EntryEvo Android Auto
             project's programs are). release/entryevo/NbtApp, release/entryevo/agent, release/entryevo/pkg

What the EntryEvo programs ask for is held to a set they may ask for, not to an exact list: the toolchain
(entrynav2-tc:4.7, GCC 4.7.2, which links with --as-needed) asks only for what is used. The two HU Framework
libraries are asked for by the plugin always. Nothing of it has been run on a car yet.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PLATFORM = os.environ.get("NBT_PLATFORM", "") or "nbtevo"
if PLATFORM not in ("nbtevo", "entryevo"):
    raise SystemExit("NBT_PLATFORM is %r: it is nbtevo or entryevo" % PLATFORM)

ENTRYEVO = PLATFORM == "entryevo"
NAME = "EntryEvo" if ENTRYEVO else "NBT Evo"

# Where a build leaves what goes to the head unit, beside the source.
RELEASE_ROOT = os.path.join(ROOT, "release", "entryevo") if ENTRYEVO else os.path.join(ROOT, "release")
SHOWN_ROOT = os.path.relpath(RELEASE_ROOT, ROOT)

# The call ABI: hard-float (Tag_ABI_VFP_args) on EntryEvo, soft-float on NBT Evo.
HARD_FLOAT = ENTRYEVO

FRAMEWORK = {"libAppFramework.A.so", "libCoreFramework.A.so"}
if ENTRYEVO:
    PLUGIN_NEEDED_EXACT = None
    PLUGIN_NEEDED_REQUIRED = FRAMEWORK
    PLUGIN_NEEDED_ALLOWED = FRAMEWORK | {"libstdc++.so.6", "libm.so.6", "libgcc_s.so.1", "libc.so.6", "libdl.so.2",
                                         "libpthread.so.0"}
    AGENT_NEEDED_EXACT = None
    AGENT_NEEDED_REQUIRED = {"libc.so.6"}
    AGENT_NEEDED_ALLOWED = {"libstdc++.so.6", "libm.so.6", "libgcc_s.so.1", "libc.so.6", "libdl.so.2",
                            "libpthread.so.0", "librt.so.1"}
    AGENT_INTERPRETER = "/lib/ld-linux-armhf.so.3"
else:
    # libsocket: the plugin's link to the agent is a local socket (src/agent_link.hpp).
    PLUGIN_NEEDED_EXACT = FRAMEWORK | {"libstdc++.so.6", "libm.so.2", "libc.so.3", "libsocket.so.3"}
    PLUGIN_NEEDED_REQUIRED = PLUGIN_NEEDED_EXACT
    PLUGIN_NEEDED_ALLOWED = PLUGIN_NEEDED_EXACT
    # What the Bluetooth tools ask for too; hu_probe.sh lists what a head unit has.
    AGENT_NEEDED_EXACT = {"libsocket.so.3", "libcpp.so.4", "libc.so.3"}
    AGENT_NEEDED_REQUIRED = AGENT_NEEDED_EXACT
    AGENT_NEEDED_ALLOWED = AGENT_NEEDED_EXACT
    AGENT_INTERPRETER = "/usr/lib/ldqnx.so.2"


def needed_problem(needed, exact, required, allowed):
    """None if a program's NEEDED is what this platform's programs may ask for, else why not."""
    if exact is not None:
        return None if needed == exact else "expected %s" % sorted(exact)
    missing, extra = sorted(required - needed), sorted(needed - allowed)
    if missing:
        return "it does not ask for %s" % missing
    if extra:
        return "%s are not among what it may ask for, %s" % (extra, sorted(allowed))
    return None


def float_abi_problem(flags, attributes):
    """None if the ELF header's flags and `readelf -A` say the call ABI of this platform, else why not."""
    hard = bool(flags & 0x400) or "Tag_ABI_VFP_args" in attributes
    if HARD_FLOAT and not hard:
        return "uses the soft-float call ABI; %s is hard-float" % NAME
    if not HARD_FLOAT and hard:
        return "uses the hard-float call ABI; %s is soft-float" % NAME
    return None
