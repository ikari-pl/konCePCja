#!/bin/bash
# Sign and notarize the macOS app bundle locally.
#
# Prerequisites (one-time setup):
#   xcrun notarytool store-credentials "koncepcja" \
#     --apple-id "your@email.com" \
#     --team-id "YOUR_TEAM_ID" \
#     --password "your-app-specific-password"
#
# Usage:
#   make ARCH=macos macos_bundle
#   ./tools/sign_and_notarize.sh

set -e

APP="release/koncepcja-macos-bundle/konCePCja.app"
DMG="release/koncepcja-macos-bundle/konCePCja.dmg"

echo "Signing $APP..."
codesign --force --deep --options runtime \
  -s "Developer ID Application" "$APP"

echo "Creating DMG..."
rm -f "$DMG"
hdiutil create -volname konCePCja -srcfolder "$APP" -ov -format UDZO "$DMG"

echo "Notarizing $DMG..."
xcrun notarytool submit "$DMG" --keychain-profile "koncepcja" --wait

echo "Stapling notarization ticket..."
xcrun stapler staple "$DMG"

echo "Done: $DMG is signed, notarized, and stapled."
