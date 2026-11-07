#!/bin/bash
# Installs the "NHL Hitz Pro" and "NHL Hitz Pro Settings" menu entries and Pro icons for this checkout.
set -e
REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"; ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
mkdir -p "$APPS" "$ICONS/64x64/apps" "$ICONS/128x128/apps" "$ICONS/256x256/apps"
cp "$REPO/linux/icons/nhlhitzpro-64.png" "$ICONS/64x64/apps/nhlhitzpro.png"
cp "$REPO/linux/icons/nhlhitzpro-128.png" "$ICONS/128x128/apps/nhlhitzpro.png"
cp "$REPO/linux/icons/nhlhitzpro-256.png" "$ICONS/256x256/apps/nhlhitzpro.png"
for f in nhl-hitz-pro nhl-hitz-pro-settings; do
  sed "s|@REPO@|$REPO|g" "$REPO/linux/$f.desktop.in" > "$APPS/$f.desktop"; chmod +x "$APPS/$f.desktop"
done
chmod +x "$REPO/linux/play.sh" "$REPO/linux/settings.sh"
echo "Installed menu entries in $APPS (Pro icon: nhlhitzpro)"
