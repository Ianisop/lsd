#!/bin/bash
cd build
mkdir shaders
mkdir fonts
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1
make

cp ../src/shaders/shader.vert shaders/
cp ../src/shaders/shader.frag shaders/
cp ../src/shaders/bg.frag shaders/
cp ../src/shaders/bg.vert shaders/

cp ../src/fonts/JetBrainsMono-Bold.ttf fonts/
cp ../src/fonts/JetBrainsMono-Italic.ttf fonts
cp ../src/fonts/JetBrainsMono-BoldItalic.ttf fonts/
cp ../src/fonts/JetBrainsMono-Medium.ttf fonts/

./lsd