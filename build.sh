#!/bin/bash
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1
make
cp ../src/shaders/shader.vert .
cp ../src/shaders/shader.frag .
cp ../src/shaders/bg.frag .
cp ../src/shaders/bg.vert .

cp ../src/fonts/JetBrainsMono-Bold.ttf .
cp ../src/fonts/JetBrainsMono-Italic.ttf .
cp ../src/fonts/JetBrainsMono-BoldItalic.ttf .
cp ../src/fonts/JetBrainsMono-Medium.ttf .
