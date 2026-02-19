#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "lsd_pty.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include "lsd.h"
#include "parse/csi_parser.h"
#include "parse/osc_parser.h"
#include "parse/ansi_parser.h"
#include "types/lsd_types.h"

namespace LSD{
    std::string WINDOW_TITLE = "lsd";
    int FONT_SIZE = 18;
    
    std::mutex g_lock;
    LSD::Types::TermState g_term;
    std::atomic<bool> g_dirty{false};
    int g_scroll_offset = 0;
    PTY pty;
    
    int g_fbWidth = LSD::WINDOW_WIDTH;
    int g_fbHeight = LSD::WINDOW_HEIGHT;
    int glyphWidth = 0;
    int glyphHeight = 0;
    
    LSD::Types::AnsiState g_ansi;
    
    GLuint g_termProgram = 0;
    GLuint g_termVAO = 0;
    GLuint g_termVBO = 0;
    std::vector<float> g_verts;
    
    GLuint g_bgProgram = 0;
    GLuint g_bgVAO = 0;
    GLuint g_bgVBO = 0;
    GLint g_bgTimeLoc = -1;
    GLint g_bgResLoc = -1;
    
    FT_Library g_ft;
    FT_Face g_face = nullptr;
    FT_Face g_bold_face = nullptr;
    FT_Face g_italic_face = nullptr;
    FT_Face g_bold_italic_face = nullptr;
    
    GLuint g_atlasTex[4] = {0, 0, 0, 0};
    
    std::map<char, LSD::Types::Glyph> g_glyphs[4];
    
    unsigned char g_atlasPx[4][LSD::ATLAS_WIDTH * LSD::ATLAS_HEIGHT];

// ─── Shader helpers ───────────────────────────────────────────────────────────
static std::string loadFile(const std::string &path) {
  std::ifstream f(path);
  if (!f) { std::cerr << "Cannot open: " << path << "\n"; return ""; }
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static GLuint compileShader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); std::cerr << log << "\n"; }
  return s;
}

// Returns 0 if either file is missing/fails — caller must check
static GLuint loadShaders(const std::string &vpath, const std::string &fpath) {
  std::string vsrc = loadFile(vpath), fsrc = loadFile(fpath);
  if (vsrc.empty() || fsrc.empty()) return 0;
  GLuint vs = compileShader(GL_VERTEX_SHADER,   vsrc.c_str());
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsrc.c_str());
  GLuint p  = glCreateProgram();
  glAttachShader(p, vs); glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[512]; glGetProgramInfoLog(p, 512, nullptr, log); std::cerr << log << "\n"; }
  glDeleteShader(vs); glDeleteShader(fs);
  return p;
}

// ─── Atlas ────────────────────────────────────────────────────────────────────
static void build_atlas(FT_Face face, std::map<char,LSD::Types::Glyph> &out, unsigned char *px) {
  if (!face) return;
  memset(px, 0, LSD::ATLAS_WIDTH * LSD::ATLAS_HEIGHT);
  out.clear();
  int x = 0, y = 0, rowH = 0;
  for (unsigned char c = 32; c < 127; ++c) {
    if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
    int w = (int)face->glyph->bitmap.width, h = (int)face->glyph->bitmap.rows;
    if (x + w >= LSD::ATLAS_WIDTH) { x = 0; y += rowH; rowH = 0; }
    if (y + h >= LSD::ATLAS_HEIGHT) { std::cerr << "Atlas overflow\n"; break; }
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i)
        px[(y+j)*LSD::ATLAS_WIDTH+(x+i)] = face->glyph->bitmap.buffer[j*face->glyph->bitmap.pitch + i];
    LSD::Types::Glyph g;
    g.u0 = float(x)/LSD::ATLAS_WIDTH;
    g.u1 = float(x+w)/LSD::ATLAS_WIDTH;
    g.v0 = float(y)/LSD::ATLAS_HEIGHT;
    g.v1 = float(y+h)/LSD::ATLAS_HEIGHT;
    if (g.u1 > 1.0f || g.v1 > 1.0f) std::cerr << "Glyph UV outside atlas\n";

    g.width = w; g.height = h;
    g.bearingX = face->glyph->bitmap_left;
    g.bearingY = face->glyph->bitmap_top;
    g.advance  = face->glyph->advance.x >> 6;
    out[(char)c] = g;
    x += w; rowH = std::max(rowH, h);
  }
}

void load_glyphs() {
  FT_Face faces[4] = { LSD::g_face, LSD::g_bold_face, LSD::g_italic_face, LSD::g_bold_italic_face };
  for (int i = 0; i < 4; ++i)
    build_atlas(faces[i], LSD::g_glyphs[i], LSD::g_atlasPx[i]);
  LSD::glyphWidth = 0; LSD::glyphHeight = 0;
  for (auto &[c,g] : LSD::g_glyphs[0]) {
    LSD::glyphWidth  = std::max(LSD::glyphWidth,  g.advance);
    LSD::glyphHeight = std::max(LSD::glyphHeight, g.height);
  }
  std::cout << "Font " << LSD::FONT_SIZE << "px  cell " << LSD::glyphWidth << "x" << LSD::glyphHeight << "\n";
}

void upload_atlases() {
  for (int i = 0; i < 4; ++i) {
    glBindTexture(GL_TEXTURE_2D, LSD::g_atlasTex[i]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, LSD::ATLAS_WIDTH, LSD::ATLAS_HEIGHT,
                 0, GL_RED, GL_UNSIGNED_BYTE, LSD::g_atlasPx[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
}

// ─── Grid helpers (call with g_lock held) ─────────────────────────────────────
void grid_resize_locked() {
  int nc = std::max(1, LSD::g_fbWidth  / std::max(LSD::glyphWidth,  1));
  int nr = std::max(1, LSD::g_fbHeight / std::max(LSD::glyphHeight, 1));
  bool empty = LSD::g_term.grid.empty();
  if (!empty && nc == LSD::g_term.cols && nr == LSD::g_term.rows) return;
  auto old = LSD::g_term.grid; int oc = LSD::g_term.cols, or_ = LSD::g_term.rows;
  LSD::g_term.cols = nc; LSD::g_term.rows = nr;
  LSD::g_term.grid.assign(nr, std::vector<LSD::Types::Cell>(nc));
  if (!empty)
    for (int r = 0; r < std::min(or_, nr); ++r)
      for (int c = 0; c < std::min(oc, nc); ++c)
        LSD::g_term.grid[r][c] = old[r][c];
  LSD::g_term.cur_col = std::clamp(LSD::g_term.cur_col, 0, nc-1);
  LSD::g_term.cur_row = std::clamp(LSD::g_term.cur_row, 0, nr-1);
  std::cout << "Grid " << nc << "x" << nr << "\n";
}

void grid_scroll_up_locked() {
  LSD::g_term.scrollback.push_back(LSD::g_term.grid.front());
  if ((int)LSD::g_term.scrollback.size() > LSD::Types::TermState::MAX_SCROLLBACK)
    LSD::g_term.scrollback.pop_front();
  LSD::g_term.grid.erase(LSD::g_term.grid.begin());
  LSD::g_term.grid.emplace_back(LSD::g_term.cols);
}

void grid_newline_locked() {
  LSD::g_term.cur_col = 0;
  if (++LSD::g_term.cur_row >= LSD::g_term.rows) {
    grid_scroll_up_locked();
    LSD::g_term.cur_row = LSD::g_term.rows - 1;
  }
}

void grid_put_locked(char c) {
  int &col = LSD::g_term.cur_col, &row = LSD::g_term.cur_row;
  const int COLS = LSD::g_term.cols, ROWS = LSD::g_term.rows;
  switch (c) {
    case '\n': LSD::grid_newline_locked(); return;
    case '\r': col = 0; return;
    case '\t': col = std::min((col/8+1)*8, COLS-1); return;
    case '\b': if (col > 0) --col; return;
    default: break;
  }
  if (c < 0x20) return;
  if (col >= COLS) { col = 0; if (++row >= ROWS) { grid_scroll_up_locked(); row = ROWS-1; } }
  if (row < 0 || row >= (int)LSD::g_term.grid.size())       return;
  if (col < 0 || col >= (int)LSD::g_term.grid[row].size()) return;
  LSD::g_term.grid[row][col] = LSD::Types::Cell{c, LSD::g_ansi.fg, LSD::g_ansi.bg, LSD::g_ansi.bold, LSD::g_ansi.italic};
  ++col;
}



// ─── PTY callback ─────────────────────────────────────────────────────────────
void read_callback(const char *msg, size_t size) {
  std::lock_guard<std::mutex> lk(LSD::g_lock);
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = (unsigned char)msg[i];
    switch (LSD::g_ansi.state) {
      case LSD::Types::EscState::Normal:
        if (byte==0x1B){LSD::g_ansi.state=LSD::Types::EscState::Esc;LSD::g_ansi.param_buf.clear();}
        else LSD::grid_put_locked((char)byte);
        break;
      case LSD::Types::EscState::Esc:
        LSD::g_ansi.param_buf.clear();
        if      (byte=='[') LSD::g_ansi.state=LSD::Types::EscState::CSI;
        else if (byte==']') LSD::g_ansi.state=LSD::Types::EscState::OSC;
        else if (byte=='c') {
          for(auto&r:LSD::g_term.grid)r.assign(LSD::g_term.cols,LSD::Types::Cell{});
          LSD::g_term.cur_row=0; 
          LSD::g_term.cur_col=0;
          LSD::g_ansi.state=LSD::Types::EscState::Normal;
        }
        else if (byte=='M') { if(LSD::g_term.cur_row>0)--LSD::g_term.cur_row; LSD::g_ansi.state=LSD::Types::EscState::Normal; }
        else LSD::g_ansi.state=LSD::Types::EscState::Normal;
        break;
      case LSD::Types::EscState::CSI:
        if (byte>=0x40&&byte<=0x7E) {
          LSD::CsiParser::process_csi_locked(LSD::g_ansi.param_buf,(char)byte);
          LSD::g_ansi.state=LSD::Types::EscState::Normal; LSD::g_ansi.param_buf.clear();
        } else LSD::g_ansi.param_buf+=(char)byte;
        break;
      case LSD::Types::EscState::OSC:
        if (byte==0x07){LSD::OscParser::process_osc(LSD::g_ansi.param_buf);LSD::g_ansi.state=LSD::Types::EscState::Normal;LSD::g_ansi.param_buf.clear();}
        else if(byte==0x1B){LSD::OscParser::process_osc(LSD::g_ansi.param_buf);LSD::g_ansi.state=LSD::Types::EscState::Esc;LSD::g_ansi.param_buf.clear();}
        else LSD::g_ansi.param_buf+=(char)byte;
        break;
    }
  }
  LSD::g_scroll_offset = 0;
  LSD::g_dirty = true;
}


// ─── Vertex builder ───────────────────────────────────────────────────────────
// Vertex layout (12 floats): x y  u v  fg.rgb  bg.rgb  isGlyph  styleIndex
// Style index:  0=regular 1=bold 2=italic 3=bolditalic  96=cellbg  98=cursorglyph  99=cursorblock
void build_terminal_vertices(std::vector<float> &verts, int W, int H) {
  verts.clear();
  if (!LSD::g_face || LSD::glyphWidth==0 || LSD::glyphHeight==0) return;

  int   ascent     = (int)(LSD::g_face->size->metrics.ascender >> 6);
  int   lineHeight = (int)(LSD::g_face->size->metrics.height   >> 6);
  float cellW = (float)LSD::glyphWidth, cellH = (float)lineHeight;

  auto ndcX = [W](float px){ return px/W*2.f-1.f; };
  auto ndcY = [H](float py){ return 1.f-py/H*2.f; };

  auto emit = [&](float vx,float vy,float vu,float vv,
                  glm::vec3 fg,glm::vec3 bg,float isG,float sI) {
    verts.insert(verts.end(),{vx,vy,vu,vv,fg.r,fg.g,fg.b,bg.r,bg.g,bg.b,isG,sI});
  };

  // Snapshot under lock
  std::vector<std::vector<LSD::Types::Cell>> snap;
  int cursorRow=0, cursorCol=0;
  LSD::Types::Cell cursorCell;
  {
    std::lock_guard<std::mutex> lk(LSD::g_lock);
    int rows   = LSD::g_term.rows;
    int offset = std::clamp(LSD::g_scroll_offset, 0, (int)LSD::g_term.scrollback.size());
    if (offset==0) {
      snap      = LSD::g_term.grid;
      cursorRow = LSD::g_term.cur_row;
    } else {
      int sbSize   = (int)LSD::g_term.scrollback.size();
      int sbStart  = sbSize - offset;
      int sbRows   = std::min(offset, rows);
      int liveRows = rows - sbRows;
      snap.reserve(rows);
      for (int i=sbStart; i<sbStart+sbRows; ++i) snap.push_back(LSD::g_term.scrollback[i]);
      for (int i=0; i<liveRows; ++i)             snap.push_back(LSD::g_term.grid[i]);
      cursorRow = sbRows + LSD::g_term.cur_row;
    }
    cursorCol = LSD::g_term.cur_col;
    int cr = LSD::g_term.cur_row;
    if (cr>=0 && cr<(int)LSD::g_term.grid.size() &&
        cursorCol>=0 && cursorCol<(int)LSD::g_term.grid[cr].size())
      cursorCell = LSD::g_term.grid[cr][cursorCol];
  }

  // Cell backgrounds + glyphs
  for (int row=0; row<(int)snap.size(); ++row)
  for (int col=0; col<(int)snap[row].size(); ++col) {
    const LSD::Types::Cell &cell = snap[row][col];
    glm::vec3 fg=cell.fg, bg=cell.bg;
    float x0=col*cellW, y0=row*cellH, x1=x0+cellW, y1=y0+cellH;
    float nx0=ndcX(x0),nx1=ndcX(x1),ny0=ndcY(y0),ny1=ndcY(y1);
    glm::vec3 z{0,0,0};

    // Semi-transparent colored bg (skip if black)
    if ((bg.r+bg.g+bg.b)>0.01f) {
      emit(nx0,ny0,0,0,bg,z,0,96.f); emit(nx1,ny0,0,0,bg,z,0,96.f); emit(nx1,ny1,0,0,bg,z,0,96.f);
      emit(nx0,ny0,0,0,bg,z,0,96.f); emit(nx1,ny1,0,0,bg,z,0,96.f); emit(nx0,ny1,0,0,bg,z,0,96.f);
    }

    char ch = cell.ch;
    if (ch==' '||ch=='\0') continue;

    // Pick glyph map
    int gi = 0;
    if      (cell.bold&&cell.italic&&LSD::g_bold_italic_face) gi=3;
    else if (cell.bold&&LSD::g_bold_face)                     gi=1;
    else if (cell.italic&&LSD::g_italic_face)                 gi=2;

    auto it = LSD::g_glyphs[gi].find(ch);
    if (it==LSD::g_glyphs[gi].end()) { it=LSD::g_glyphs[0].find(ch); gi=0; }
    if (it==LSD::g_glyphs[0].end()) continue;
    const LSD::Types::Glyph &g = it->second;

    float gx0=x0+g.bearingX, gy0=y0+ascent-g.bearingY;
    float gx1=gx0+g.width,   gy1=gy0+g.height;
    float ngx0=ndcX(gx0),ngx1=ndcX(gx1),ngy0=ndcY(gy0),ngy1=ndcY(gy1);
    float sI=(float)gi;

    emit(ngx0,ngy0,g.u0,g.v0,fg,bg,1,sI); emit(ngx1,ngy0,g.u1,g.v0,fg,bg,1,sI); emit(ngx1,ngy1,g.u1,g.v1,fg,bg,1,sI);
    emit(ngx0,ngy0,g.u0,g.v0,fg,bg,1,sI); emit(ngx1,ngy1,g.u1,g.v1,fg,bg,1,sI); emit(ngx0,ngy1,g.u0,g.v1,fg,bg,1,sI);
  }

// Cursor (live view only)
if (LSD::cursor_visible() && LSD::g_scroll_offset == 0) {
    float x0 = cursorCol * cellW;
    float y0 = cursorRow * cellH;
    float x1 = x0 + cellW;
    float y1 = y0 + cellH;

    // Convert to NDC
    float nx0 = ndcX(x0), nx1 = ndcX(x1);
    float ny0 = ndcY(y0), ny1 = ndcY(y1);

    glm::vec3 white{1,1,1}, dummy{0,0,0};

    // Draw solid white cursor block
    emit(nx0, ny0, 0,0, white, dummy, 0, 99.f); emit(nx1, ny0, 0,0, white, dummy, 0, 99.f);
    emit(nx1, ny1, 0,0, white, dummy, 0, 99.f); emit(nx0, ny0, 0,0, white, dummy, 0, 99.f);
    emit(nx1, ny1, 0,0, white, dummy, 0, 99.f); emit(nx0, ny1, 0,0, white, dummy, 0, 99.f);

    // Cursor glyph — black ink
    char ch = cursorCell.ch;
    if (ch != ' ' && ch != '\0') {
        int gi = 0; // default atlas
        if      (cursorCell.bold && cursorCell.italic && LSD::g_bold_italic_face) gi=3;
        else if (cursorCell.bold && LSD::g_bold_face) gi=1;
        else if (cursorCell.italic && LSD::g_italic_face) gi=2;

        auto it = LSD::g_glyphs[gi].find(ch);
        if (it == LSD::g_glyphs[gi].end()) { it = LSD::g_glyphs[0].find(ch); gi = 0; }
        if (it != LSD::g_glyphs[gi].end()) {
            const LSD::Types::Glyph &g = it->second;

            // Use correct face ascent for this glyph
            int ascent = 0;
            if      (gi==0 && LSD::g_face)             ascent = LSD::g_face->size->metrics.ascender >> 6;
            else if (gi==1 && LSD::g_bold_face)        ascent = LSD::g_bold_face->size->metrics.ascender >> 6;
            else if (gi==2 && LSD::g_italic_face)      ascent = LSD::g_italic_face->size->metrics.ascender >> 6;
            else if (gi==3 && LSD::g_bold_italic_face) ascent = LSD::g_bold_italic_face->size->metrics.ascender >> 6;

            float gx0 = x0 + g.bearingX;
            float gy0 = y0 + ascent - g.bearingY;
            float gx1 = gx0 + g.width;
            float gy1 = gy0 + g.height;

            // Convert glyph coords to NDC
            float ngx0 = ndcX(gx0), ngx1 = ndcX(gx1);
            float ngy0 = ndcY(gy0), ngy1 = ndcY(gy1);

            emit(ngx0, ngy0, g.u0, g.v0, white, dummy, 1, 98.f); 
            emit(ngx1, ngy0, g.u1, g.v0, white, dummy, 1, 98.f);
            emit(ngx1, ngy1, g.u1, g.v1, white, dummy, 1, 98.f);

            emit(ngx0, ngy0, g.u0, g.v0, white, dummy, 1, 98.f);
            emit(ngx1, ngy1, g.u1, g.v1, white, dummy, 1, 98.f);
            emit(ngx0, ngy1, g.u0, g.v1, white, dummy, 1, 98.f);
        }
    }
}


}

void upload_vbo() {
  glBindBuffer(GL_ARRAY_BUFFER, LSD::g_termVBO);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(LSD::g_verts.size()*sizeof(float)), LSD::g_verts.data(), GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─── Font resize ──────────────────────────────────────────────────────────────
void reload_font_size(int sz) {
  if (sz<LSD::FONT_SIZE_MIN||sz>LSD::FONT_SIZE_MAX) return;
  LSD::FONT_SIZE=sz;
  auto ss=[](FT_Face f){if(f)FT_Set_Pixel_Sizes(f,0,(FT_UInt)LSD::FONT_SIZE);};
  ss(LSD::g_face);ss(LSD::g_bold_face);ss(LSD::g_italic_face);ss(LSD::g_bold_italic_face);
  load_glyphs(); upload_atlases();
  {std::lock_guard<std::mutex> lk(LSD::g_lock);LSD::grid_resize_locked();}
  LSD::pty.resize(LSD::g_term.cols,LSD::g_term.rows); LSD::g_dirty=true;
}

// ─── Callbacks ───────────────────────────────────────────────────────────────
void framebuffer_size_callback(GLFWwindow*,int w,int h){
  if(w<=0||h<=0) return;
  glViewport(0,0,w,h); LSD::g_fbWidth=w; LSD::g_fbHeight=h;
  {std::lock_guard<std::mutex> lk(LSD::g_lock);LSD::grid_resize_locked();}
  LSD::pty.resize(LSD::g_term.cols,LSD::g_term.rows); LSD::g_dirty=true;
}

static void key_callback(GLFWwindow *win,int key,int,int action,int mods){
  if(action!=GLFW_PRESS&&action!=GLFW_REPEAT) return;
  if(key==GLFW_KEY_ESCAPE){glfwSetWindowShouldClose(win,GL_TRUE);return;}
  if(LSD::g_scroll_offset!=0){LSD::g_scroll_offset=0;LSD::g_dirty=true;}
  if(mods&GLFW_MOD_CONTROL){
    switch(key){
      case GLFW_KEY_C:{char v=0x03;LSD::pty.write(&v,1);return;}
      case GLFW_KEY_D:{char v=0x04;LSD::pty.write(&v,1);return;}
      case GLFW_KEY_L:{char v=0x0C;LSD::pty.write(&v,1);return;}
      case GLFW_KEY_Z:{char v=0x1A;LSD::pty.write(&v,1);return;}
      case GLFW_KEY_EQUAL:case GLFW_KEY_KP_ADD:    reload_font_size(LSD::FONT_SIZE+2);return;
      case GLFW_KEY_MINUS:case GLFW_KEY_KP_SUBTRACT:reload_font_size(LSD::FONT_SIZE-2);return;
      case GLFW_KEY_0:case GLFW_KEY_KP_0:           reload_font_size(18);return;
      default:break;
    }
  }
  if(mods&GLFW_MOD_SUPER){
    switch(key){
      case GLFW_KEY_EQUAL:case GLFW_KEY_KP_ADD:    reload_font_size(LSD::FONT_SIZE+2);return;
      case GLFW_KEY_MINUS:case GLFW_KEY_KP_SUBTRACT:reload_font_size(LSD::FONT_SIZE-2);return;
      case GLFW_KEY_0:case GLFW_KEY_KP_0:           reload_font_size(18);return;
      default:break;
    }
  }
  const char *seq=nullptr;
  switch(key){
    case GLFW_KEY_ENTER:    {const char v[]="\r";   LSD::pty.write(v,1);return;}
    case GLFW_KEY_BACKSPACE:{const char v[]="\x7f"; LSD::pty.write(v,1);return;}
    case GLFW_KEY_TAB:      {const char v[]="\t";   LSD::pty.write(v,1);return;}
    case GLFW_KEY_UP:        seq="\033[A";  break;
    case GLFW_KEY_DOWN:      seq="\033[B";  break;
    case GLFW_KEY_RIGHT:     seq="\033[C";  break;
    case GLFW_KEY_LEFT:      seq="\033[D";  break;
    case GLFW_KEY_HOME:      seq="\033[H";  break;
    case GLFW_KEY_END:       seq="\033[F";  break;
    case GLFW_KEY_PAGE_UP:   seq="\033[5~"; break;
    case GLFW_KEY_PAGE_DOWN: seq="\033[6~"; break;
    case GLFW_KEY_DELETE:    seq="\033[3~"; break;
    case GLFW_KEY_INSERT:    seq="\033[2~"; break;
    default:break;
  }
  if(seq) LSD::pty.write(seq,strlen(seq));
}

bool cursor_visible() {
    // Simple implementation - cursor blinks every 0.5 seconds
    static double lastToggle = 0.0;
    static bool visible = true;
    double now = glfwGetTime();
    
    if (now - lastToggle > 0.5) {
        visible = !visible;
        lastToggle = now;
    }
    return visible;
}

static void char_callback(GLFWwindow*,unsigned int cp){
  if(cp<0x80){char c=(char)cp;LSD::pty.write(&c,1);}
}

static void scroll_callback(GLFWwindow*,double,double yoff){
  static double accum=0.0; accum+=yoff;
  int max_offset;
  {std::lock_guard<std::mutex> lk(LSD::g_lock);max_offset=(int)LSD::g_term.scrollback.size();}
  if(max_offset==0){accum=0;return;}
  int step=(int)accum; if(step==0) return; accum-=step;
  {std::lock_guard<std::mutex> lk(LSD::g_lock);LSD::g_scroll_offset=std::clamp(LSD::g_scroll_offset-step,0,max_offset);}
  LSD::g_dirty=true;
}
}
int main(){
  if(!glfwInit()) return -1;
  glfwWindowHint(GLFW_DECORATED,GLFW_TRUE);
  GLFWwindow *win=glfwCreateWindow(LSD::WINDOW_WIDTH,LSD::WINDOW_HEIGHT,LSD::WINDOW_TITLE.c_str(),nullptr,nullptr);
  if(!win) return -1;
  glfwMakeContextCurrent(win);
  if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

  int fbW,fbH; glfwGetFramebufferSize(win,&fbW,&fbH);
  LSD::g_fbWidth=fbW; LSD::g_fbHeight=fbH;
  glViewport(0,0,fbW,fbH);
  glfwSetFramebufferSizeCallback(win,LSD::framebuffer_size_callback);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(0.05f,0.05f,0.05f,1.f);

  // ── Terminal shader (required) ───────────────────────────────────────────
  LSD::g_termProgram = LSD::loadShaders("shader.vert","shader.frag");
  if(!LSD::g_termProgram){ std::cerr<<"Failed to load terminal shaders\n"; return -1; }

  // ── Background shader (optional — falls back to clear colour) ────────────
  LSD::g_bgProgram = LSD::loadShaders("bg.vert","bg.frag");
  if(LSD::g_bgProgram){
    LSD::g_bgTimeLoc = glGetUniformLocation(LSD::g_bgProgram,"uTime");
    LSD::g_bgResLoc  = glGetUniformLocation(LSD::g_bgProgram,"uResolution");

    static const float bgV[] = {
      -1.f,  1.f,  0.f,0.f,
       1.f,  1.f,  1.f,0.f,
       1.f, -1.f,  1.f,1.f,
      -1.f,  1.f,  0.f,0.f,
       1.f, -1.f,  1.f,1.f,
      -1.f, -1.f,  0.f,1.f,
    };
    glGenVertexArrays(1,&LSD::g_bgVAO);
    glGenBuffers(1,&LSD::g_bgVBO);
    glBindVertexArray(LSD::g_bgVAO);
      glBindBuffer(GL_ARRAY_BUFFER,LSD::g_bgVBO);
      glBufferData(GL_ARRAY_BUFFER,sizeof(bgV),bgV,GL_STATIC_DRAW);
      glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
      glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glBindBuffer(GL_ARRAY_BUFFER,0);
    glBindVertexArray(0);
    std::cout<<"Background shader loaded ok\n";
  } else {
    std::cerr<<"bg.vert/bg.frag not found — using clear colour\n";
  }

  // ── Fonts ────────────────────────────────────────────────────────────────
  if(FT_Init_FreeType(&LSD::g_ft)){std::cerr<<"FT init failed\n";return -1;}
  if(FT_New_Face(LSD::g_ft,"JetBrainsMono-Medium.ttf",0,&LSD::g_face)){std::cerr<<"Regular font missing\n";return -1;}
  FT_New_Face(LSD::g_ft,"JetBrainsMono-Bold.ttf",      0,&LSD::g_bold_face);
  FT_New_Face(LSD::g_ft,"JetBrainsMono-Italic.ttf",    0,&LSD::g_italic_face);
  FT_New_Face(LSD::g_ft,"JetBrainsMono-BoldItalic.ttf",0,&LSD::g_bold_italic_face);

  auto setSize=[](FT_Face f){if(f)FT_Set_Pixel_Sizes(f,0,(FT_UInt)LSD::FONT_SIZE);};
  setSize(LSD::g_face);setSize(LSD::g_bold_face);setSize(LSD::g_italic_face);setSize(LSD::g_bold_italic_face);
  LSD::load_glyphs();

  glGenTextures(4,LSD::g_atlasTex);
  LSD::upload_atlases();

  {std::lock_guard<std::mutex> lk(LSD::g_lock);LSD::grid_resize_locked();}

  // ── Terminal shader uniforms ──────────────────────────────────────────────
  glUseProgram(LSD::g_termProgram);
  glUniform1i(glGetUniformLocation(LSD::g_termProgram,"atlasRegular"),   0);
  glUniform1i(glGetUniformLocation(LSD::g_termProgram,"atlasBold"),      1);
  glUniform1i(glGetUniformLocation(LSD::g_termProgram,"atlasItalic"),    2);
  glUniform1i(glGetUniformLocation(LSD::g_termProgram,"atlasBoldItalic"),3);
  glUseProgram(0);

  // ── Terminal VAO (12 floats per vertex) ──────────────────────────────────
  const GLsizei STRIDE = 12*sizeof(float);
  glGenVertexArrays(1,&LSD::g_termVAO);
  glGenBuffers(1,&LSD::g_termVBO);
  glBindVertexArray(LSD::g_termVAO);
    glBindBuffer(GL_ARRAY_BUFFER,LSD::g_termVBO);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,STRIDE,(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,STRIDE,(void*)(2*sizeof(float)));
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,STRIDE,(void*)(4*sizeof(float)));
    glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,STRIDE,(void*)(7*sizeof(float)));
    glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,STRIDE,(void*)(10*sizeof(float)));
    glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,STRIDE,(void*)(11*sizeof(float)));
    for(int i=0;i<6;++i) glEnableVertexAttribArray(i);
    LSD::build_terminal_vertices(LSD::g_verts,fbW,fbH);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(LSD::g_verts.size()*sizeof(float)),LSD::g_verts.data(),GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER,0);
  glBindVertexArray(0);

  glfwSetKeyCallback(win,   LSD::key_callback);
  glfwSetCharCallback(win,  LSD::char_callback);
  glfwSetScrollCallback(win,LSD::scroll_callback);
  LSD::pty.setReadCallback(LSD::read_callback);
 LSD:: pty.spawn(LSD::g_term.cols,LSD::g_term.rows);

  static bool lastBlink=false;

  while(!glfwWindowShouldClose(win)){
    if(LSD::g_dirty.exchange(false)){
      LSD::build_terminal_vertices(LSD::g_verts,LSD::g_fbWidth,LSD::g_fbHeight);
      LSD::upload_vbo();
    }
    bool blink=LSD::cursor_visible();
    if(blink!=lastBlink){LSD::g_dirty=true;lastBlink=blink;}

    float now=(float)glfwGetTime();
    glClear(GL_COLOR_BUFFER_BIT);

    // 1) Background
    if(LSD::g_bgProgram){
      glUseProgram(LSD::g_bgProgram);
      if(LSD::g_bgTimeLoc>=0) glUniform1f(LSD::g_bgTimeLoc,now);
      if(LSD::g_bgResLoc>=0)  glUniform2f(LSD::g_bgResLoc,(float)LSD::g_fbWidth,(float)LSD::g_fbHeight);
      glBindVertexArray(LSD::g_bgVAO);
      glDrawArrays(GL_TRIANGLES,0,6);
      glBindVertexArray(0);
    }

    // 2) Terminal
    glUseProgram(LSD::g_termProgram);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,LSD::g_atlasTex[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,LSD::g_atlasTex[1]);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,LSD::g_atlasTex[2]);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D,LSD::g_atlasTex[3]);
    glBindVertexArray(LSD::g_termVAO);
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)(LSD::g_verts.size()/12));
    glBindVertexArray(0);

    glfwSwapBuffers(win);
    glfwPollEvents();
  }

  LSD::pty.stop();
  if(LSD::g_face)            FT_Done_Face(LSD::g_face);
  if(LSD::g_bold_face)       FT_Done_Face(LSD::g_bold_face);
  if(LSD::g_italic_face)     FT_Done_Face(LSD::g_italic_face);
  if(LSD::g_bold_italic_face)FT_Done_Face(LSD::g_bold_italic_face);
  FT_Done_FreeType(LSD::g_ft);
  glfwTerminate();
  return 0;
}