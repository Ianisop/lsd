#pragma once
#include <iostream>

#include <glm/common.hpp>
#include "lsd_pty.h"
// DONT include csi_parser.h here, it causes circular includes
#include "types/lsd_types.h"
#include <GLFW/glfw3.h>
#include <mutex>
#include <atomic>
#include "clip.h"
#include <map>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace LSD
{
// Constants (keep static)
static const int WINDOW_WIDTH = 800;
static const int WINDOW_HEIGHT = 600;
static const int FONT_SIZE_MIN = 8;
static const int FONT_SIZE_MAX = 72;
static const int ATLAS_WIDTH = 512;
static const int ATLAS_HEIGHT = 512;
static const int MAX_FPS = 240 + 4;// +4 to adjust for the schedulers bullshit

// Variables that need to be shared (extern)
extern std::string WINDOW_TITLE;
extern int FONT_SIZE;
extern std::mutex lock;
extern Types::TerminalState *current_terminal_state;
extern LSD::Types::TerminalState terminal_states[];
extern std::atomic<bool> dirt_flag;
extern int scroll_offset;
extern PTY *current_pty;
extern int g_fbWidth, g_fbHeight;
extern int glyph_width, glyph_height;
extern Types::AnsiState ansi_state;
extern double delta_time;


// OpenGL stuff
extern GLuint g_terminal_program, g_terminal_VAO, g_terminal_VBO;
extern std::vector<float> g_vertices;
extern GLuint g_background_program, g_background_VAO, g_background_VBO;
extern GLint g_background_time_loc, g_background_res_Loc;

// Font stuff
extern FT_Library font_library;
extern FT_Face font_normal_face, font_bold_face, font_italic_face, font_bold_italic_face;
extern GLuint atlas_textures[4];
extern std::map<char, Types::Glyph> glyphs[4];
extern unsigned char atlas_px[4][ATLAS_WIDTH * ATLAS_HEIGHT];

// Function declarations
void loadGylphs();
void uploadAtlases();
void buildTerminalVertices(std::vector<float> &verts, int W, int H);
void uploadVbo();
bool cursorVisible();
void gridScrollUpLocked();
void gridResizeLocked();
void gridNewlineLocked();
void gridPutLocked(char c);
}// namespace LSD
