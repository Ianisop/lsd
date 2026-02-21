#!/bin/bash
set -e

git clone https://github.com/Ianisop/lsd
cd lsd

APP_NAME="lsd"
BUILD_DIR="build"
INSTALL_BIN="/usr/local/bin/$APP_NAME"
INSTALL_SHARE="/usr/local/share/$APP_NAME"
DESKTOP_FILE="/usr/share/applications/$APP_NAME.desktop"

# Build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make
cd ..

# Install executable
echo "Installing executable to $INSTALL_BIN"
sudo cp "$BUILD_DIR/$APP_NAME" "$INSTALL_BIN"
sudo chmod +x "$INSTALL_BIN"

# Install resources
echo "Installing resources to $INSTALL_SHARE"
sudo mkdir -p "$INSTALL_SHARE"
sudo mkdir -p "$INSTALL_SHARE/shaders"
sudo mkdir -p "$INSTALL_SHARE/fonts"
sudo cp src/shaders/*.vert "$INSTALL_SHARE/shaders/"
sudo cp src/shaders/*.frag "$INSTALL_SHARE/shaders/"
sudo cp src/fonts/*.ttf "$INSTALL_SHARE/fonts/"
sudo cp src/lsd.png /usr/local/share/lsd/lsd.png

# Create .desktop file
echo "Creating .desktop file at $DESKTOP_FILE"
sudo tee "$DESKTOP_FILE" > /dev/null <<EOF
[Desktop Entry]
Name=$APP_NAME
Comment=Lightweight terminal app
Exec=$INSTALL_BIN
Icon=/usr/local/share/lsd/lsd.png
Terminal=false
Type=Application
Categories=Utility;TerminalEmulator;
EOF

cd ..
rm -rf lsd/

echo "Installation complete. You can run '$APP_NAME' from your application menu or terminal."
