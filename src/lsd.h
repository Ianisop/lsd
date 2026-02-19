#pragma once
#include <iostream>
#include <glm/common.hpp>
#include "lsd_pty.h"
// DON'T include csi_parser.h here - it causes circular includes
#include "types/lsd_types.h"
#include <GLFW/glfw3.h>
#include <mutex>
#include <atomic>
#include <map>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace LSD
{
    // Constants (keep static)
    static const int WINDOW_WIDTH  = 800;
    static const int WINDOW_HEIGHT = 600;
    static const int FONT_SIZE_MIN = 8;
    static const int FONT_SIZE_MAX = 72;
    static const int ATLAS_WIDTH  = 512;
    static const int ATLAS_HEIGHT = 512;
    
    // Variables that need to be shared (extern)
    extern std::string WINDOW_TITLE;
    extern int FONT_SIZE;
    extern std::mutex g_lock;
    extern Types::TermState g_term;  // This is the key declaration
    extern std::atomic<bool> g_dirty;
    extern int g_scroll_offset;
    extern PTY pty;
    extern int g_fbWidth, g_fbHeight;
    extern int glyphWidth, glyphHeight;
    extern Types::AnsiState g_ansi;
    
    // OpenGL stuff
    extern GLuint g_termProgram, g_termVAO, g_termVBO;
    extern std::vector<float> g_verts;
    extern GLuint g_bgProgram, g_bgVAO, g_bgVBO;
    extern GLint g_bgTimeLoc, g_bgResLoc;
    
    // Font stuff
    extern FT_Library g_ft;
    extern FT_Face g_face, g_bold_face, g_italic_face, g_bold_italic_face;
    extern GLuint g_atlasTex[4];
    extern std::map<char, Types::Glyph> g_glyphs[4];
    extern unsigned char g_atlasPx[4][ATLAS_WIDTH * ATLAS_HEIGHT];
    
    // Function declarations
    void load_glyphs();
    void upload_atlases();
    void build_terminal_vertices(std::vector<float> &verts, int W, int H);
    void upload_vbo();
    bool cursor_visible();
    void grid_scroll_up_locked();
    void grid_resize_locked();
    void grid_newline_locked();
    void grid_put_locked(char c);
}
