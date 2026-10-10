#!/bin/bash
#  Copyright 2026 Paul Schulze (Hel Industries), All rights reserved.
#
#  Builds HIAudioStreamer as a universal macOS app, deploys Qt into the bundle, signs it ad hoc
#  and packages it into a disk image in the build directory.
#
#  Usage: Tools/Build.sh [build directory]
#      build directory   default ../Build/Mac beside this checkout
#      QT_ROOT_DIR       the macOS Qt kit, default /Users/Shared/Qt/6.9.3/macos
#
#  @variable(g_source) the checkout this script belongs to
#  @variable(g_build) where CMake builds, and where the disk image lands
#  @variable(g_qt) the Qt kit
#  @variable(g_app) the staged app bundle

g_source="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
g_build="${1:-${g_source}/../Build/Mac}"
g_qt="${QT_ROOT_DIR:-/Users/Shared/Qt/6.9.3/macos}"
g_app="${g_build}/Deploy/HIAudioStreamer.app"

# Stops the build, naming the step that failed and where.
# @param step: what was being done
# @param code: the exit code to leave with
function build_fail()
{
    local _step="$1"
    local _code="$2"
    echo "Build.sh: ${_step} failed (${FUNCNAME[1]}, ${BASH_SOURCE[1]}:${BASH_LINENO[0]})" >&2
    exit "${_code}"
}

# Configures, builds and installs the bundle into the staging directory.
function build_app()
{
    cmake -S "${g_source}" -B "${g_build}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${g_qt}" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        || build_fail "configure" 2
    cmake --build "${g_build}" || build_fail "compile" 3
    rm -rf "${g_build}/Deploy"
    cmake --install "${g_build}" --prefix "${g_build}/Deploy" || build_fail "install" 4
}

# Puts the licences and Qt into the bundle, then signs it. macdeployqt brings along the virtual
# keyboard input plugin, and with it QML, Quick and OpenGL, none of which the tray menu uses;
# those go before signing, since anything changed afterwards breaks the seal.
function build_deploy()
{
    local _contents="${g_app}/Contents"

    mkdir -p "${_contents}/Resources/Licenses"
    cp "${g_source}/LICENSE" "${g_source}/Documentation/Licenses/"* \
        "${_contents}/Resources/Licenses/" || build_fail "copy licences" 5
    "${g_qt}/bin/macdeployqt" "${g_app}" || build_fail "macdeployqt" 6
    rm -rf "${_contents}/PlugIns/platforminputcontexts" \
        "${_contents}/Frameworks/"Qt{OpenGL,Qml,QmlMeta,QmlModels,QmlWorkerScript,Quick}.framework \
        "${_contents}/Frameworks/"QtVirtualKeyboard{,Qml}.framework
    codesign --force --deep --sign - "${g_app}" || build_fail "codesign" 7
    codesign --verify --deep --strict "${g_app}" || build_fail "verify signature" 7
}

# Packages the bundle into a compressed disk image with a link to /Applications to drag it to.
function build_dmg()
{
    local _version
    _version="$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" \
        "${g_app}/Contents/Info.plist")" || build_fail "read version" 8
    local _staging="${g_build}/Dmg"
    local _dmg="${g_build}/HIAudioStreamer-${_version}-macos.dmg"

    rm -rf "${_staging}" "${_dmg}"
    mkdir -p "${_staging}"
    cp -R "${g_app}" "${_staging}/" || build_fail "stage disk image" 9
    ln -s /Applications "${_staging}/Applications"
    hdiutil create -volname "HIAudioStreamer ${_version}" -srcfolder "${_staging}" -format UDZO \
        -ov "${_dmg}" || build_fail "hdiutil" 10
    rm -rf "${_staging}"
    echo "${_dmg}"
}

build_app
build_deploy
build_dmg
