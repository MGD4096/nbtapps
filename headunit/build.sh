#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Builds release/NbtApp/: the library in the QNX SDP 6.5.0 Docker image (qcc, GCC 4.4.2, armv7le), then the
# manifest, XML and text / image DBs on the host (python3), then the plugin's own package of that folder ->
# release/pkg/ (not signed), then `make check`.
#
# usage: ./build.sh          # build + check
#        ./build.sh agent    # the agent: its tests (in the bench container), then built for the head unit -> release/agent/,
#                            # its own package -> release/pkg/ (not signed), and `make agent-check`
#        ./build.sh clean    # remove build/ and release/
#
# Environment:
#   QNX_LICENSE_DIR  (required) directory holding your QNX `licenses` file; mounted at /etc/qnx/license.
#                    It must be writable: qcc creates licenses.lck next to the file.
#   QNX_IMAGE        Docker image with the SDP (default: qnx650-sdk)
#
# EntryEvo (EntryNav2): PLATFORM=entryevo ./build.sh [agent] does the same into release/entryevo/, with GCC 4.7 armhf
# in the image entrynav2-tc:4.7 instead of the SDP; no QNX licence is needed.
#   ENTRYEVO_IMAGE   Docker image with the toolchain (default: entrynav2-tc:4.7, which
#                    `docker build -t entrynav2-tc:4.7 <EntryEvo-Android-Auto>/toolchain` makes)
#   HU_ROOTFS        a copy of an EntryEvo head unit's system, mounted read-only at /sysroot: the plugin links
#                    against its HU Framework (/opt/hmi/lib) and libraries. Default ~/entrynav2/rootfs. The agent
#                    needs none
set -eu

image=${QNX_IMAGE:-qnx650-sdk}
platform=${PLATFORM:-nbtevo}

cd "$(dirname "$0")"

if [ "${1:-}" = clean ]; then
    exec make clean
fi
target=lib
if [ "${1:-}" = agent ]; then
    target=$1
elif [ $# -gt 0 ]; then
    echo "usage: [PLATFORM=nbtevo|entryevo] $0 [agent|clean]" >&2
    exit 1
fi

if [ "$platform" = entryevo ]; then
    image=${ENTRYEVO_IMAGE:-entrynav2-tc:4.7}
    rootfs=${HU_ROOTFS:-$HOME/entrynav2/rootfs}
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        echo "build.sh: Docker image '$image' not found: docker build -t $image <EntryEvo-Android-Auto>/toolchain" >&2
        exit 1
    fi
    if [ "$target" = lib ] && [ ! -d "$rootfs/opt/hmi/lib" ]; then
        echo "build.sh: no EntryEvo system copy in '$rootfs' (it needs opt/hmi/lib): set HU_ROOTFS" >&2
        exit 1
    fi
    mounts=
    if [ -d "$rootfs" ]; then mounts="-v $(cd "$rootfs" && pwd):/sysroot:ro"; fi
    version=$(git describe --always --dirty 2>/dev/null || echo unknown)
    if [ "$target" = agent ]; then
        make agent-test
    fi
    # shellcheck disable=SC2086
    docker run --rm -u "$(id -u):$(id -g)" -v "$(pwd)":/work $mounts -w /work "$image" \
        make PLATFORM=entryevo "$target" VERSION="$version"
    if [ "$target" = lib ]; then
        make PLATFORM=entryevo pkg
        make PLATFORM=entryevo plugin-pkg
        make PLATFORM=entryevo check
    else
        make PLATFORM=entryevo agent-pkg
        make PLATFORM=entryevo agent-check
    fi
    exit 0
elif [ "$platform" != nbtevo ]; then
    echo "build.sh: PLATFORM is nbtevo or entryevo" >&2
    exit 1
fi

if [ -z "${QNX_LICENSE_DIR:-}" ]; then
    echo "build.sh: set QNX_LICENSE_DIR to the directory holding your QNX 'licenses' file" >&2
    exit 1
fi
if [ ! -f "$QNX_LICENSE_DIR/licenses" ]; then
    echo "build.sh: no licenses file in '$QNX_LICENSE_DIR'" >&2
    exit 1
fi
if [ ! -w "$QNX_LICENSE_DIR" ]; then
    echo "build.sh: '$QNX_LICENSE_DIR' is not writable (qcc needs it for licenses.lck)" >&2
    exit 1
fi
if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "build.sh: Docker image '$image' not found (set QNX_IMAGE)" >&2
    exit 1
fi

# The image has no git, so the version is worked out here.
version=$(git describe --always --dirty 2>/dev/null || echo unknown)

if [ "$target" = agent ]; then
    make agent-test
fi
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$(pwd)":/src \
    -v "$(cd "$QNX_LICENSE_DIR" && pwd)":/etc/qnx/license \
    "$image" make "$target" VERSION="$version"
if [ "$target" = lib ]; then
    make pkg
    make plugin-pkg
    make check
elif [ "$target" = agent ]; then
    make agent-pkg
    make agent-check
fi
