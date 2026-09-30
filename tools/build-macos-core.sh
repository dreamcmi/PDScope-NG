#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
: "${TARGET_BUILD_DIR:?TARGET_BUILD_DIR must be supplied by Xcode}"
: "${FRAMEWORKS_FOLDER_PATH:?FRAMEWORKS_FOLDER_PATH must be supplied by Xcode}"

if command -v cmake >/dev/null 2>&1; then
  cmake_bin=$(command -v cmake)
elif [ -x /opt/homebrew/bin/cmake ]; then
  cmake_bin=/opt/homebrew/bin/cmake
elif [ -x /usr/local/bin/cmake ]; then
  cmake_bin=/usr/local/bin/cmake
else
  printf '%s\n' 'CMake was not found in PATH, /opt/homebrew/bin, or /usr/local/bin.' >&2
  exit 1
fi

archs=${ARCHS:-$(uname -m)}
configuration=${CONFIGURATION:-Release}
deployment_target=${MACOSX_DEPLOYMENT_TARGET:-10.15}
sdkroot=${SDKROOT:-}
build_jobs=${PDSCOPE_BUILD_JOBS:-3}
arch_list=$(printf '%s' "$archs" | tr ';' ' ' | awk '{$1=$1; gsub(/ /, ";"); print}')
arch_key=$(printf '%s' "$archs" | tr ' ;' '__')
build_dir="$repo_root/build/macos-core/$arch_key/$configuration"

case "$build_jobs" in
  ''|*[!0-9]*)
    printf '%s\n' 'PDSCOPE_BUILD_JOBS must be a positive integer.' >&2
    exit 1
    ;;
esac
case "$build_jobs" in
  *[1-9]*) ;;
  *)
    printf '%s\n' 'PDSCOPE_BUILD_JOBS must be a positive integer.' >&2
    exit 1
    ;;
esac

set -- -S "$repo_root" \
  -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$configuration" \
  -DCMAKE_OSX_ARCHITECTURES="$arch_list" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target"
if [ -n "$sdkroot" ]; then
  set -- "$@" "-DCMAKE_OSX_SYSROOT=$sdkroot"
fi

"$cmake_bin" "$@"
"$cmake_bin" --build "$build_dir" --target pdscope --config "$configuration" --parallel "$build_jobs"

library="$build_dir/out/libpdscope.dylib"
if [ ! -f "$library" ]; then
  library="$build_dir/out/$configuration/libpdscope.dylib"
fi
if [ ! -f "$library" ]; then
  printf 'CMake completed without producing the expected library: %s\n' "$library" >&2
  exit 1
fi

frameworks_dir="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH"
mkdir -p "$frameworks_dir"
destination="$frameworks_dir/libpdscope.dylib"
cp -f "$library" "$destination"

sign_identity=${EXPANDED_CODE_SIGN_IDENTITY:--}
/usr/bin/codesign --force --sign "$sign_identity" "$destination"
