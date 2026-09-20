#!/usr/bin/env bash
# Builds and installs the Quest port.  No Gradle: the app is a NativeActivity
# with no Java, so aapt2 + apksigner are enough.
#
#   ./build.sh          build, package, install
#   ./build.sh build    build and package only
#   ./build.sh run      install and launch
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
ABI="arm64-v8a"
MIN_SDK=29
TARGET_SDK=32
PACKAGE="com.snes9x.vr"
VERSION_NAME="0.1"
VERSION_CODE=2
OPENXR_VERSION="1.1.63"

BUILD_DIR="$HERE/build"
STAGE_DIR="$BUILD_DIR/stage"
OPENXR_DIR="$HERE/third_party/openxr"

pick_latest() { ls "$1" | sort -V | tail -1; }

NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$(pick_latest "$SDK/ndk")}"
BUILD_TOOLS="$SDK/build-tools/$(pick_latest "$SDK/build-tools")"
PLATFORM_JAR="$SDK/platforms/android-$(ls "$SDK/platforms" | sed 's/android-//' | sort -V | tail -1)/android.jar"
CMAKE_BIN="$SDK/cmake/$(pick_latest "$SDK/cmake")/bin/cmake"
NINJA_BIN="$SDK/cmake/$(pick_latest "$SDK/cmake")/bin/ninja"
ADB="$SDK/platform-tools/adb"

# --- OpenXR loader ----------------------------------------------------------
fetch_openxr() {
    [ -f "$OPENXR_DIR/libs/$ABI/libopenxr_loader.so" ] && return

    echo ">> fetching OpenXR loader $OPENXR_VERSION"
    local tmp
    tmp="$(mktemp -d)"
    curl -sfL -o "$tmp/oxr.aar" \
        "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_VERSION/openxr_loader_for_android-$OPENXR_VERSION.aar"
    unzip -qo "$tmp/oxr.aar" -d "$tmp/oxr"

    mkdir -p "$OPENXR_DIR/include" "$OPENXR_DIR/libs/$ABI"
    cp -r "$tmp/oxr/prefab/modules/headers/include/openxr" "$OPENXR_DIR/include/"
    cp "$tmp/oxr/jni/$ABI/libopenxr_loader.so" "$OPENXR_DIR/libs/$ABI/"
    rm -rf "$tmp"
}

# --- native build -----------------------------------------------------------
build_native() {
    echo ">> building native code"
    "$CMAKE_BIN" -S "$HERE/cpp" -B "$BUILD_DIR/cmake" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$MIN_SDK" \
        -DANDROID_NDK="$NDK" \
        -DSNES9X_VR_VERSION="$VERSION_NAME" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    "$CMAKE_BIN" --build "$BUILD_DIR/cmake" --parallel
}

# --- packaging --------------------------------------------------------------
# Release builds are signed with a key of their own, kept outside the repo.
# Android refuses to install an update signed by a different key, so this one
# has to survive: losing it means every future version needs an uninstall
# first.  Debug builds use the usual debug key.
RELEASE_KEYSTORE="$HOME/.android/snes9xvr-release.keystore"
RELEASE_PASSFILE="$HOME/.android/snes9xvr-release.pass"

keystore() {
    if [ "${RELEASE_BUILD:-0}" = "1" ] && [ -f "$RELEASE_KEYSTORE" ]; then
        echo "$RELEASE_KEYSTORE"
        return
    fi

    local ks="$HOME/.android/debug.keystore"
    if [ ! -f "$ks" ]; then
        mkdir -p "$HOME/.android"
        keytool -genkeypair -keystore "$ks" -storepass android -keypass android \
            -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10950 \
            -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
    fi
    echo "$ks"
}

keystore_pass() {
    if [ "${RELEASE_BUILD:-0}" = "1" ] && [ -f "$RELEASE_KEYSTORE" ]; then
        cat "$RELEASE_PASSFILE"
        return
    fi
    echo "android"
}

keystore_alias() {
    if [ "${RELEASE_BUILD:-0}" = "1" ] && [ -f "$RELEASE_KEYSTORE" ]; then
        echo "snes9xvr"
        return
    fi
    echo "androiddebugkey"
}

package() {
    echo ">> packaging APK"
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR/lib/$ABI"

    # The C++ runtime is linked statically, so only these two ship.
    cp "$BUILD_DIR/cmake/libsnes9xvr.so" "$STAGE_DIR/lib/$ABI/"
    cp "$OPENXR_DIR/libs/$ABI/libopenxr_loader.so" "$STAGE_DIR/lib/$ABI/"
    "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
        "$STAGE_DIR/lib/$ABI/libsnes9xvr.so"

    "$BUILD_TOOLS/aapt2" link \
        -o "$BUILD_DIR/base.apk" \
        --manifest "$HERE/AndroidManifest.xml" \
        -I "$PLATFORM_JAR" \
        --min-sdk-version "$MIN_SDK" \
        --target-sdk-version "$TARGET_SDK" \
        --version-code "$VERSION_CODE" --version-name "$VERSION_NAME"

    (cd "$STAGE_DIR" && zip -qr "$BUILD_DIR/base.apk" lib)

    "$BUILD_TOOLS/zipalign" -f 4 "$BUILD_DIR/base.apk" "$BUILD_DIR/aligned.apk"
    local pass
    pass="$(keystore_pass)"

    "$BUILD_TOOLS/apksigner" sign \
        --ks "$(keystore)" --ks-key-alias "$(keystore_alias)" \
        --ks-pass "pass:$pass" --key-pass "pass:$pass" \
        --out "$BUILD_DIR/snes9xvr.apk" "$BUILD_DIR/aligned.apk"

    echo ">> $BUILD_DIR/snes9xvr.apk"
}

install_apk() {
    echo ">> installing"
    "$ADB" install -r "$BUILD_DIR/snes9xvr.apk"

    # The app creates its own directories on first run; they have to belong to
    # the app rather than to the shell user, so let it start once first.
    "$ADB" shell am start -n "$PACKAGE/android.app.NativeActivity" >/dev/null
    sleep 2
    "$ADB" shell am force-stop "$PACKAGE"
    echo ">> push ROMs with:"
    echo "   adb push game.sfc /sdcard/Android/data/$PACKAGE/files/roms/"
}

launch() {
    "$ADB" shell am start -n "$PACKAGE/android.app.NativeActivity"
}

testrom() {
    local rom="$BUILD_DIR/test.sfc"
    mkdir -p "$BUILD_DIR"
    python3 "$HERE/tools/make_test_rom.py" "$rom"
    "$ADB" push "$rom" "/sdcard/Android/data/$PACKAGE/files/roms/"
}

release() {
    if [ ! -f "$RELEASE_KEYSTORE" ]; then
        echo ">> creating the release key at $RELEASE_KEYSTORE"
        mkdir -p "$(dirname "$RELEASE_KEYSTORE")"
        # Generated once and kept: see the comment above keystore().
        head -c 24 /dev/urandom | base64 | tr -d '\n=/+' > "$RELEASE_PASSFILE"
        chmod 600 "$RELEASE_PASSFILE"
        keytool -genkeypair -keystore "$RELEASE_KEYSTORE" \
            -storepass "$(cat "$RELEASE_PASSFILE")" \
            -keypass "$(cat "$RELEASE_PASSFILE")" \
            -alias snes9xvr -keyalg RSA -keysize 4096 -validity 10950 \
            -dname "CN=Snes9x VR,O=Snes9x VR,C=DE" >/dev/null 2>&1
    fi

    RELEASE_BUILD=1
    export RELEASE_BUILD

    fetch_openxr
    build_native
    package

    local out="$BUILD_DIR/Snes9xVR-$VERSION_NAME.apk"
    cp "$BUILD_DIR/snes9xvr.apk" "$out"
    "$BUILD_TOOLS/apksigner" verify --print-certs "$out" | head -3
    echo ">> release $VERSION_NAME: $out ($(stat -c%s "$out") bytes)"
}

case "${1:-all}" in
    build) fetch_openxr; build_native; package ;;
    release) release ;;
    testrom) testrom ;;
    run)   install_apk; launch ;;
    all)   fetch_openxr; build_native; package; install_apk ;;
    *)     echo "usage: $0 [build|run|testrom|release|all]"; exit 1 ;;
esac
