#!/usr/bin/env bash
# Builds the macOS DMG: an installer package (installs the VST3 into
# /Library/Audio/Plug-Ins/VST3) plus the plug-in itself for drag-and-drop install.
#
# Usage: packaging/macos/make_dmg.sh <path/to/Plugin.vst3> <output-dir>
# Optional: SIGN_APP_ID / SIGN_INSTALLER_ID (Developer ID identities) to sign.
set -euo pipefail

VST3="$1"
OUT="${2:-dist}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

NAME="$(basename "$VST3" .vst3)"
VERSION="$(sed -nE 's/^project\([^ ]+ VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
BUNDLE_ID="$(sed -nE 's/^set\(BUNDLE_ID +"([^"]+)".*/\1/p' "$ROOT/CMakeLists.txt")"
SAFE_NAME="${NAME// /}"
WORK="$(mktemp -d)"
mkdir -p "$OUT"

echo "Packaging $NAME $VERSION ($BUNDLE_ID)"

# Optional code signing of the plug-in (ad-hoc signature from the build otherwise)
if [ -n "${SIGN_APP_ID:-}" ]; then
  codesign --force --deep --options runtime --timestamp --sign "$SIGN_APP_ID" "$VST3"
fi

# 1. Component package with a fixed install location
mkdir -p "$WORK/root/Library/Audio/Plug-Ins/VST3"
ditto "$VST3" "$WORK/root/Library/Audio/Plug-Ins/VST3/$NAME.vst3"
pkgbuild --analyze --root "$WORK/root" "$WORK/components.plist"
# Never let the installer "relocate" the bundle to some other copy it finds on disk
plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/components.plist"
pkgbuild --root "$WORK/root" \
         --component-plist "$WORK/components.plist" \
         --identifier "$BUNDLE_ID.vst3" \
         --version "$VERSION" \
         --install-location / \
         "$WORK/$SAFE_NAME-VST3.pkg"

# 2. Product installer with a welcome page and licence notes
mkdir -p "$WORK/resources"
cp "$HERE/welcome.html" "$WORK/resources/welcome.html"
cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>$NAME $VERSION</title>
  <welcome file="welcome.html" mime-type="text/html"/>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <volume-check><allowed-os-versions><os-version min="11.0"/></allowed-os-versions></volume-check>
  <choices-outline><line choice="vst3"/></choices-outline>
  <choice id="vst3" title="VST3 plug-in"><pkg-ref id="$BUNDLE_ID.vst3"/></choice>
  <pkg-ref id="$BUNDLE_ID.vst3" version="$VERSION" onConclusion="none">$SAFE_NAME-VST3.pkg</pkg-ref>
</installer-gui-script>
EOF
PRODUCT_ARGS=(--distribution "$WORK/distribution.xml" --resources "$WORK/resources" --package-path "$WORK")
if [ -n "${SIGN_INSTALLER_ID:-}" ]; then
  PRODUCT_ARGS+=(--sign "$SIGN_INSTALLER_ID")
fi
productbuild "${PRODUCT_ARGS[@]}" "$WORK/Install $NAME.pkg"

# 3. Disk image
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp "$WORK/Install $NAME.pkg" "$STAGE/"
ditto "$VST3" "$STAGE/$NAME.vst3"
ln -s /Library/Audio/Plug-Ins/VST3 "$STAGE/VST3 Plug-Ins folder"
cp "$HERE/ReadMe.txt" "$STAGE/Read Me.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/Third-Party Notices.txt"

DMG="$OUT/$SAFE_NAME-$VERSION-macOS.dmg"
rm -f "$DMG"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG"
if [ -n "${SIGN_APP_ID:-}" ]; then
  codesign --force --timestamp --sign "$SIGN_APP_ID" "$DMG"
fi

# Sanity checks: the image mounts and contains what we expect
MNT="$(mktemp -d)"
hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$DMG" >/dev/null
test -d "$MNT/$NAME.vst3/Contents/MacOS"
test -f "$MNT/Install $NAME.pkg"
pkgutil --payload-files "$MNT/Install $NAME.pkg" | grep -q "Library/Audio/Plug-Ins/VST3/$NAME.vst3/Contents/MacOS"
lipo -archs "$MNT/$NAME.vst3/Contents/MacOS/$NAME"
hdiutil detach "$MNT" >/dev/null

echo "Created $DMG"
