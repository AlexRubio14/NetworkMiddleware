#!/usr/bin/env bash
# NetworkMiddleware Server — Linux installer
# Usage: curl -fsSL https://raw.githubusercontent.com/AlexRubio14/NetworkMiddleware/main/install.sh | bash

set -e

REPO="AlexRubio14/NetworkMiddleware"
INSTALL_DIR="$HOME/.local/bin"

echo "NetworkMiddleware Server — installer"
echo "Fetching latest release..."

URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" \
      | grep "browser_download_url.*AppImage" \
      | cut -d '"' -f 4)

if [ -z "$URL" ]; then
    echo "Error: no AppImage found in latest release." >&2
    exit 1
fi

echo "Downloading $(basename "$URL")..."
curl -Lo /tmp/NetServer.AppImage "$URL"
chmod +x /tmp/NetServer.AppImage

mkdir -p "$INSTALL_DIR"
mv /tmp/NetServer.AppImage "$INSTALL_DIR/NetServer"

# Add ~/.local/bin to PATH if not already there
if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
    echo "export PATH=\"\$PATH:$INSTALL_DIR\"" >> "$HOME/.bashrc"
    echo "Note: restart your terminal or run: export PATH=\"\$PATH:$INSTALL_DIR\""
fi

echo ""
echo "Done! Run: NetServer"
