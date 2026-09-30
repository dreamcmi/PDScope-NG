#!/bin/sh
# Run after Flutter embeds App.framework, before Xcode's final signing step.
set -eu

: "${TARGET_BUILD_DIR:?TARGET_BUILD_DIR must be supplied by Xcode}"
: "${WRAPPER_NAME:?WRAPPER_NAME must be supplied by Xcode}"
bundle="$TARGET_BUILD_DIR/$WRAPPER_NAME"

# Only remove Finder detritus. Other xattrs can contain nested bundle signatures.
for attribute in com.apple.FinderInfo com.apple.ResourceFork; do
  /usr/bin/xattr -dr "$attribute" "$bundle" 2>/dev/null || true
done

if [ "${CODE_SIGNING_ALLOWED:-YES}" = NO ]; then
  exit 0
fi

set -- --force --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}"
if [ -n "${CODE_SIGN_ENTITLEMENTS:-}" ]; then
  case "$CODE_SIGN_ENTITLEMENTS" in
    /*) entitlements="$CODE_SIGN_ENTITLEMENTS" ;;
    *) entitlements="$PROJECT_DIR/$CODE_SIGN_ENTITLEMENTS" ;;
  esac
  set -- "$@" --entitlements "$entitlements"
fi

# Dart-only incremental builds can change App.framework without relinking Runner.
# Refresh the outer seal as well, so the bundle remains valid after any build.
/usr/bin/codesign "$@" "$bundle"
