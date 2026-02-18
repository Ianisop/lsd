<h1 align="center">lsd</h1>
<p align="center"><b>Lightweight Shader-Driven Terminal Emulator</b></p>

<p align="center">
  <img src="https://img.shields.io/badge/version-0.1.0-blue">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue">
  <img src="https://img.shields.io/badge/OpenGL-3.3%2B-green">
  <img src="https://img.shields.io/badge/platform-Linux-orange">
  <img src="https://img.shields.io/badge/status-experimental-purple">
</p>

---

## ✦ Overview

**lsd** is a modern GPU-accelerated terminal emulator written in **C++20** using **OpenGL**.  
It renders terminal text as a real-time graphics workload instead of a traditional UI surface.

## ✦ Build

```bash
git clone https://github.com/yourname/lsd.git
cd lsd
mkdir build && cd build
cmake ..
make -j$(nproc)
./lsd
```

![ss](https://cdn.discordapp.com/attachments/1107275969166843995/1473589628018757724/image.png?ex=6996c2c9&is=69957149&hm=d964bf520b01a4208426584468a4c3f39092df5b7a30cd220bafd19b4423b7c2&)
