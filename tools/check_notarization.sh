#!/bin/bash
# Check the status of a notarization submission.
#
# Prerequisites (one-time setup):
#   xcrun notarytool store-credentials "koncepcja" \
#     --apple-id "your@email.com" \
#     --team-id "YOUR_TEAM_ID" \
#     --password "your-app-specific-password"
#
# Usage:
#   ./tools/check_notarization.sh                    # lists recent submissions
#   ./tools/check_notarization.sh <submission-id>    # checks specific submission

set -e

PROFILE="koncepcja"

if [ -z "$1" ]; then
  echo "Recent submissions:"
  xcrun notarytool history --keychain-profile "$PROFILE"
else
  echo "Checking submission $1..."
  xcrun notarytool info "$1" --keychain-profile "$PROFILE"
fi
