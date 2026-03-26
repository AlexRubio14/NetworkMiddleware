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

# Add ~/.local/bin to PATH in shell rc files if not already present
PATH_LINE="export PATH=\"\$PATH:$INSTALL_DIR\""
_added=0
for _rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [ -f "$_rc" ] && ! grep -qF "$INSTALL_DIR" "$_rc"; then
        echo "$PATH_LINE" >> "$_rc"
        _added=1
    fi
done
# Also update ~/.profile so login shells (including fish, etc.) pick it up
if [ -f "$HOME/.profile" ] && ! grep -qF "$INSTALL_DIR" "$HOME/.profile"; then
    echo "$PATH_LINE" >> "$HOME/.profile"
    _added=1
fi
if [ "$_added" -eq 1 ]; then
    echo "Note: restart your terminal or run: export PATH=\"\$PATH:$INSTALL_DIR\""
fi

echo ""
echo "Done! Run: NetServer"
