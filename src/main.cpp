#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <ft2build.h>
#include FT_FREETYPE_H
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

// ─── Constants ────────────────────────────────────────────────────────────────
static const int WINDOW_WIDTH  = 800;
static const int WINDOW_HEIGHT = 600;
std::string WINDOW_TITLE = "lsd";

static const int FONT_SIZE_MIN = 8;
static const int FONT_SIZE_MAX = 72;
int FONT_SIZE = 18;

static const int ATLAS_WIDTH  = 512;
static const int ATLAS_HEIGHT = 512;

// ─── Cell ─────────────────────────────────────────────────────────────────────
struct Cell {
  char      ch     = ' ';
  glm::vec3 fg{1.f, 1.f, 1.f};
  glm::vec3 bg{0.f, 0.f, 0.f};
  bool      bold = false, italic = false;
};

// ─── Terminal state ───────────────────────────────────────────────────────────
struct TermState {
  std::vector<std::vector<Cell>> grid;
  int cols = 80, rows = 24, cur_col = 0, cur_row = 0;
  std::deque<std::vector<Cell>> scrollback;
  static constexpr int MAX_SCROLLBACK = 5000;
};

static std::mutex        g_lock;
static TermState         g_term;
static std::atomic<bool> g_dirty{false};
static int               g_scroll_offset = 0;
PTY pty;

// ─── Globals ──────────────────────────────────────────────────────────────────
int g_fbWidth = WINDOW_WIDTH, g_fbHeight = WINDOW_HEIGHT;
int glyphWidth = 0, glyphHeight = 0;

// Terminal rendering — 12-float vertex stride
GLuint g_termProgram = 0;
GLuint g_termVAO     = 0;
GLuint g_termVBO     = 0;
std::vector<float> g_verts;

// Background rendering — 4-float vertex stride (xy + uv), optional
GLuint g_bgProgram = 0;
GLuint g_bgVAO     = 0;
GLuint g_bgVBO     = 0;
GLint  g_bgTimeLoc = -1;
GLint  g_bgResLoc  = -1;

// Fonts & atlases
FT_Library g_ft;
FT_Face g_face             = nullptr;
FT_Face g_bold_face        = nullptr;
FT_Face g_italic_face      = nullptr;
FT_Face g_bold_italic_face = nullptr;

GLuint g_atlasTex[4]; // 0=regular 1=bold 2=italic 3=bolditalic

struct Glyph {
  float u0, v0, u1, v1;
  int   width, height, bearingX, bearingY, advance;
};
std::map<char,Glyph> g_glyphs[4]; // same order as atlas

static unsigned char g_atlasPx[4][ATLAS_WIDTH * ATLAS_HEIGHT];

// ─── ANSI parser ──────────────────────────────────────────────────────────────
enum class EscState { Normal, Esc, CSI, OSC };
struct AnsiState {
  EscState    state = EscState::Normal;
  std::string param_buf;
  glm::vec3   fg{0.f, 0.8f, 0.6f}, bg{0.f, 0.f, 0.f};
  bool        bold = false, italic = false;
};
static AnsiState g_ansi;

// ─── Forward decls ────────────────────────────────────────────────────────────
void load_glyphs();
void upload_atlases();
void build_terminal_vertices(std::vector<float> &verts, int W, int H);
void upload_vbo();

// ─── Colour ───────────────────────────────────────────────────────────────────
static glm::vec3 ansi_to_rgb(int n) {
  switch(n) {
    case 30: return {0,0,0};         case 31: return {.8f,0,0};
    case 32: return {0,.8f,0};       case 33: return {.8f,.8f,0};
    case 34: return {0,0,.8f};       case 35: return {.8f,0,.8f};
    case 36: return {0,.8f,.8f};     case 37: return {.8f,.8f,.8f};
    case 90: return {.5f,.5f,.5f};   case 91: return {1,0,0};
    case 92: return {0,1,0};         case 93: return {1,1,0};
    case 94: return {.3f,.3f,1};     case 95: return {1,0,1};
    case 96: return {0,1,1};         case 97: return {1,1,1};
    default: return {1,1,1};
  }
}

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
static void build_atlas(FT_Face face, std::map<char,Glyph> &out, unsigned char *px) {
  if (!face) return;
  memset(px, 0, ATLAS_WIDTH * ATLAS_HEIGHT);
  out.clear();
  int x = 0, y = 0, rowH = 0;
  for (unsigned char c = 32; c < 127; ++c) {
    if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
    int w = (int)face->glyph->bitmap.width, h = (int)face->glyph->bitmap.rows;
    if (x + w >= ATLAS_WIDTH) { x = 0; y += rowH; rowH = 0; }
    if (y + h >= ATLAS_HEIGHT) { std::cerr << "Atlas overflow\n"; break; }
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i)
        px[(y+j)*ATLAS_WIDTH+(x+i)] = face->glyph->bitmap.buffer[j*face->glyph->bitmap.pitch + i];
    Glyph g;
    g.u0 = float(x)/ATLAS_WIDTH;
    g.u1 = float(x+w)/ATLAS_WIDTH;
    g.v0 = float(y)/ATLAS_HEIGHT;
    g.v1 = float(y+h)/ATLAS_HEIGHT;
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
  FT_Face faces[4] = { g_face, g_bold_face, g_italic_face, g_bold_italic_face };
  for (int i = 0; i < 4; ++i)
    build_atlas(faces[i], g_glyphs[i], g_atlasPx[i]);
  glyphWidth = 0; glyphHeight = 0;
  for (auto &[c,g] : g_glyphs[0]) {
    glyphWidth  = std::max(glyphWidth,  g.advance);
    glyphHeight = std::max(glyphHeight, g.height);
  }
  std::cout << "Font " << FONT_SIZE << "px  cell " << glyphWidth << "x" << glyphHeight << "\n";
}

void upload_atlases() {
  for (int i = 0; i < 4; ++i) {
    glBindTexture(GL_TEXTURE_2D, g_atlasTex[i]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, ATLAS_WIDTH, ATLAS_HEIGHT,
                 0, GL_RED, GL_UNSIGNED_BYTE, g_atlasPx[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
}

// ─── Grid helpers (call with g_lock held) ─────────────────────────────────────
static void grid_resize_locked() {
  int nc = std::max(1, g_fbWidth  / std::max(glyphWidth,  1));
  int nr = std::max(1, g_fbHeight / std::max(glyphHeight, 1));
  bool empty = g_term.grid.empty();
  if (!empty && nc == g_term.cols && nr == g_term.rows) return;
  auto old = g_term.grid; int oc = g_term.cols, or_ = g_term.rows;
  g_term.cols = nc; g_term.rows = nr;
  g_term.grid.assign(nr, std::vector<Cell>(nc));
  if (!empty)
    for (int r = 0; r < std::min(or_, nr); ++r)
      for (int c = 0; c < std::min(oc, nc); ++c)
        g_term.grid[r][c] = old[r][c];
  g_term.cur_col = std::clamp(g_term.cur_col, 0, nc-1);
  g_term.cur_row = std::clamp(g_term.cur_row, 0, nr-1);
  std::cout << "Grid " << nc << "x" << nr << "\n";
}

static void grid_scroll_up_locked() {
  g_term.scrollback.push_back(g_term.grid.front());
  if ((int)g_term.scrollback.size() > TermState::MAX_SCROLLBACK)
    g_term.scrollback.pop_front();
  g_term.grid.erase(g_term.grid.begin());
  g_term.grid.emplace_back(g_term.cols);
}

static void grid_newline_locked() {
  g_term.cur_col = 0;
  if (++g_term.cur_row >= g_term.rows) {
    grid_scroll_up_locked();
    g_term.cur_row = g_term.rows - 1;
  }
}

static void grid_put_locked(char c) {
  int &col = g_term.cur_col, &row = g_term.cur_row;
  const int COLS = g_term.cols, ROWS = g_term.rows;
  switch (c) {
    case '\n': grid_newline_locked(); return;
    case '\r': col = 0; return;
    case '\t': col = std::min((col/8+1)*8, COLS-1); return;
    case '\b': if (col > 0) --col; return;
    default: break;
  }
  if (c < 0x20) return;
  if (col >= COLS) { col = 0; if (++row >= ROWS) { grid_scroll_up_locked(); row = ROWS-1; } }
  if (row < 0 || row >= (int)g_term.grid.size())       return;
  if (col < 0 || col >= (int)g_term.grid[row].size()) return;
  g_term.grid[row][col] = Cell{c, g_ansi.fg, g_ansi.bg, g_ansi.bold, g_ansi.italic};
  ++col;
}

// ─── ANSI / VT ────────────────────────────────────────────────────────────────
static void apply_sgr(const std::string &params) {
  if (params.empty() || params == "0") {
    g_ansi.fg = {1,1,1}; g_ansi.bg = {0,0,0};
    g_ansi.bold = false; g_ansi.italic = false; return;
  }
  std::stringstream ss(params); std::string tok;
  while (std::getline(ss, tok, ';')) {
    if (tok.empty()) tok = "0";
    int n = 0; try { n = std::stoi(tok); } catch (...) { continue; }
    switch (n) {
      case 0: g_ansi.fg={0,.8f,.6f}; g_ansi.bg={0,0,0}; g_ansi.bold=false; g_ansi.italic=false; break;
      case 1: g_ansi.bold=true;   break; case 3: g_ansi.italic=true;  break;
      case 22:g_ansi.bold=false;  break; case 23:g_ansi.italic=false; break;
      case 39:g_ansi.fg={0,.8f,.6f}; break; case 49:g_ansi.bg={0,0,0}; break;
      default:
        if ((n>=30&&n<=37)||(n>=90&&n<=97)) g_ansi.fg = ansi_to_rgb(n);
        else if (n>=40&&n<=47)  g_ansi.bg = ansi_to_rgb(n-10);
        else if (n>=100&&n<=107)g_ansi.bg = ansi_to_rgb(n-70);
        break;
    }
  }
}

static void process_csi_locked(const std::string &params, char fb) {
  auto parse = [&](int def) -> std::vector<int> {
    std::vector<int> v; std::stringstream ss(params); std::string t;
    while (std::getline(ss, t, ';')) v.push_back(t.empty()||t=="0" ? def : std::stoi(t));
    if (v.empty()) v.push_back(def); return v;
  };
  auto P = [&](int i, int def) -> int { auto v=parse(def); return i<(int)v.size()?v[i]:def; };
  if (!params.empty() && params[0] == '?') return;
  int &col = g_term.cur_col, &row = g_term.cur_row;
  const int C = g_term.cols, R = g_term.rows;
  auto &grid = g_term.grid;
  row = std::clamp(row,0,R-1); col = std::clamp(col,0,C-1);
  switch (fb) {
    case 'm': apply_sgr(params); break;
    case 'A': row=std::max(0,row-P(0,1)); break;
    case 'B': row=std::min(R-1,row+P(0,1)); break;
    case 'C': col=std::min(C-1,col+P(0,1)); break;
    case 'D': col=std::max(0,col-P(0,1)); break;
    case 'E': row=std::min(R-1,row+P(0,1)); col=0; break;
    case 'F': row=std::max(0,row-P(0,1)); col=0; break;
    case 'G': col=std::clamp(P(0,1)-1,0,C-1); break;
    case 'd': row=std::clamp(P(0,1)-1,0,R-1); break;
    case 'H': case 'f': {
      auto v=parse(1);
      row=std::clamp((v.size()>0?v[0]:1)-1,0,R-1);
      col=std::clamp((v.size()>1?v[1]:1)-1,0,C-1);
      break;
    }
    case 'J': {
      int n=P(0,0);
      if(n==0){for(int c=col;c<C;++c)grid[row][c]=Cell{};for(int r=row+1;r<R;++r)grid[r].assign(C,Cell{});}
      else if(n==1){for(int r=0;r<row;++r)grid[r].assign(C,Cell{});for(int c=0;c<=col&&c<C;++c)grid[row][c]=Cell{};}
      else if(n==2||n==3){for(auto&r:grid)r.assign(C,Cell{});row=0;col=0;}
      break;
    }
    case 'K': {
      int n=P(0,0);
      if(n==0)for(int c=col;c<C;++c)grid[row][c]=Cell{};
      else if(n==1)for(int c=0;c<=col&&c<C;++c)grid[row][c]=Cell{};
      else if(n==2)grid[row].assign(C,Cell{});
      break;
    }
    case 'L': for(int i=0;i<P(0,1);++i){grid.insert(grid.begin()+row,std::vector<Cell>(C));if((int)grid.size()>R)grid.resize(R);} break;
    case 'M': for(int i=0;i<P(0,1);++i){if(row<(int)grid.size())grid.erase(grid.begin()+row);grid.emplace_back(C);} break;
    case 'S': for(int i=0;i<P(0,1);++i)grid_scroll_up_locked(); break;
    case 'T': for(int i=0;i<P(0,1);++i){grid.pop_back();grid.insert(grid.begin(),std::vector<Cell>(C));} break;
    case 'X': for(int i=0;i<P(0,1)&&col+i<C;++i)grid[row][col+i]=Cell{}; break;
    case 'h': case 'l': case 'r': break;
    default: break;
  }
}

static void process_osc(const std::string &seq) {
  size_t sep = seq.find(';'); if (sep==std::string::npos) return;
  int cmd=-1; try{cmd=std::stoi(seq.substr(0,sep));}catch(...){return;}
  if (cmd==0||cmd==2) WINDOW_TITLE=seq.substr(sep+1);
}

// ─── PTY callback ─────────────────────────────────────────────────────────────
void read_callback(const char *msg, size_t size) {
  std::lock_guard<std::mutex> lk(g_lock);
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = (unsigned char)msg[i];
    switch (g_ansi.state) {
      case EscState::Normal:
        if (byte==0x1B){g_ansi.state=EscState::Esc;g_ansi.param_buf.clear();}
        else grid_put_locked((char)byte);
        break;
      case EscState::Esc:
        g_ansi.param_buf.clear();
        if      (byte=='[') g_ansi.state=EscState::CSI;
        else if (byte==']') g_ansi.state=EscState::OSC;
        else if (byte=='c') {
          for(auto&r:g_term.grid)r.assign(g_term.cols,Cell{});
          g_term.cur_row=0; g_term.cur_col=0;
          g_ansi.state=EscState::Normal;
        }
        else if (byte=='M') { if(g_term.cur_row>0)--g_term.cur_row; g_ansi.state=EscState::Normal; }
        else g_ansi.state=EscState::Normal;
        break;
      case EscState::CSI:
        if (byte>=0x40&&byte<=0x7E) {
          process_csi_locked(g_ansi.param_buf,(char)byte);
          g_ansi.state=EscState::Normal; g_ansi.param_buf.clear();
        } else g_ansi.param_buf+=(char)byte;
        break;
      case EscState::OSC:
        if (byte==0x07){process_osc(g_ansi.param_buf);g_ansi.state=EscState::Normal;g_ansi.param_buf.clear();}
        else if(byte==0x1B){process_osc(g_ansi.param_buf);g_ansi.state=EscState::Esc;g_ansi.param_buf.clear();}
        else g_ansi.param_buf+=(char)byte;
        break;
    }
  }
  g_scroll_offset = 0;
  g_dirty = true;
}

bool cursor_visible() { return ((int)(glfwGetTime()*2.0))%2==0; }

// ─── Vertex builder ───────────────────────────────────────────────────────────
// Vertex layout (12 floats): x y  u v  fg.rgb  bg.rgb  isGlyph  styleIndex
// Style index:  0=regular 1=bold 2=italic 3=bolditalic  96=cellbg  98=cursorglyph  99=cursorblock
void build_terminal_vertices(std::vector<float> &verts, int W, int H) {
  verts.clear();
  if (!g_face || glyphWidth==0 || glyphHeight==0) return;

  int   ascent     = (int)(g_face->size->metrics.ascender >> 6);
  int   lineHeight = (int)(g_face->size->metrics.height   >> 6);
  float cellW = (float)glyphWidth, cellH = (float)lineHeight;

  auto ndcX = [W](float px){ return px/W*2.f-1.f; };
  auto ndcY = [H](float py){ return 1.f-py/H*2.f; };

  auto emit = [&](float vx,float vy,float vu,float vv,
                  glm::vec3 fg,glm::vec3 bg,float isG,float sI) {
    verts.insert(verts.end(),{vx,vy,vu,vv,fg.r,fg.g,fg.b,bg.r,bg.g,bg.b,isG,sI});
  };

  // Snapshot under lock
  std::vector<std::vector<Cell>> snap;
  int cursorRow=0, cursorCol=0;
  Cell cursorCell;
  {
    std::lock_guard<std::mutex> lk(g_lock);
    int rows   = g_term.rows;
    int offset = std::clamp(g_scroll_offset, 0, (int)g_term.scrollback.size());
    if (offset==0) {
      snap      = g_term.grid;
      cursorRow = g_term.cur_row;
    } else {
      int sbSize   = (int)g_term.scrollback.size();
      int sbStart  = sbSize - offset;
      int sbRows   = std::min(offset, rows);
      int liveRows = rows - sbRows;
      snap.reserve(rows);
      for (int i=sbStart; i<sbStart+sbRows; ++i) snap.push_back(g_term.scrollback[i]);
      for (int i=0; i<liveRows; ++i)             snap.push_back(g_term.grid[i]);
      cursorRow = sbRows + g_term.cur_row;
    }
    cursorCol = g_term.cur_col;
    int cr = g_term.cur_row;
    if (cr>=0 && cr<(int)g_term.grid.size() &&
        cursorCol>=0 && cursorCol<(int)g_term.grid[cr].size())
      cursorCell = g_term.grid[cr][cursorCol];
  }

  // Cell backgrounds + glyphs
  for (int row=0; row<(int)snap.size(); ++row)
  for (int col=0; col<(int)snap[row].size(); ++col) {
    const Cell &cell = snap[row][col];
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
    if      (cell.bold&&cell.italic&&g_bold_italic_face) gi=3;
    else if (cell.bold&&g_bold_face)                     gi=1;
    else if (cell.italic&&g_italic_face)                 gi=2;

    auto it = g_glyphs[gi].find(ch);
    if (it==g_glyphs[gi].end()) { it=g_glyphs[0].find(ch); gi=0; }
    if (it==g_glyphs[0].end()) continue;
    const Glyph &g = it->second;

    float gx0=x0+g.bearingX, gy0=y0+ascent-g.bearingY;
    float gx1=gx0+g.width,   gy1=gy0+g.height;
    float ngx0=ndcX(gx0),ngx1=ndcX(gx1),ngy0=ndcY(gy0),ngy1=ndcY(gy1);
    float sI=(float)gi;

    emit(ngx0,ngy0,g.u0,g.v0,fg,bg,1,sI); emit(ngx1,ngy0,g.u1,g.v0,fg,bg,1,sI); emit(ngx1,ngy1,g.u1,g.v1,fg,bg,1,sI);
    emit(ngx0,ngy0,g.u0,g.v0,fg,bg,1,sI); emit(ngx1,ngy1,g.u1,g.v1,fg,bg,1,sI); emit(ngx0,ngy1,g.u0,g.v1,fg,bg,1,sI);
  }

// Cursor (live view only)
if (cursor_visible() && g_scroll_offset == 0) {
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
        if      (cursorCell.bold && cursorCell.italic && g_bold_italic_face) gi=3;
        else if (cursorCell.bold && g_bold_face) gi=1;
        else if (cursorCell.italic && g_italic_face) gi=2;

        auto it = g_glyphs[gi].find(ch);
        if (it == g_glyphs[gi].end()) { it = g_glyphs[0].find(ch); gi = 0; }
        if (it != g_glyphs[gi].end()) {
            const Glyph &g = it->second;

            // Use correct face ascent for this glyph
            int ascent = 0;
            if      (gi==0 && g_face)             ascent = g_face->size->metrics.ascender >> 6;
            else if (gi==1 && g_bold_face)        ascent = g_bold_face->size->metrics.ascender >> 6;
            else if (gi==2 && g_italic_face)      ascent = g_italic_face->size->metrics.ascender >> 6;
            else if (gi==3 && g_bold_italic_face) ascent = g_bold_italic_face->size->metrics.ascender >> 6;

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
  glBindBuffer(GL_ARRAY_BUFFER, g_termVBO);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_verts.size()*sizeof(float)), g_verts.data(), GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─── Font resize ──────────────────────────────────────────────────────────────
void reload_font_size(int sz) {
  if (sz<FONT_SIZE_MIN||sz>FONT_SIZE_MAX) return;
  FONT_SIZE=sz;
  auto ss=[](FT_Face f){if(f)FT_Set_Pixel_Sizes(f,0,(FT_UInt)FONT_SIZE);};
  ss(g_face);ss(g_bold_face);ss(g_italic_face);ss(g_bold_italic_face);
  load_glyphs(); upload_atlases();
  {std::lock_guard<std::mutex> lk(g_lock);grid_resize_locked();}
  pty.resize(g_term.cols,g_term.rows); g_dirty=true;
}

// ─── Callbacks ───────────────────────────────────────────────────────────────
void framebuffer_size_callback(GLFWwindow*,int w,int h){
  if(w<=0||h<=0) return;
  glViewport(0,0,w,h); g_fbWidth=w; g_fbHeight=h;
  {std::lock_guard<std::mutex> lk(g_lock);grid_resize_locked();}
  pty.resize(g_term.cols,g_term.rows); g_dirty=true;
}

static void key_callback(GLFWwindow *win,int key,int,int action,int mods){
  if(action!=GLFW_PRESS&&action!=GLFW_REPEAT) return;
  if(key==GLFW_KEY_ESCAPE){glfwSetWindowShouldClose(win,GL_TRUE);return;}
  if(g_scroll_offset!=0){g_scroll_offset=0;g_dirty=true;}
  if(mods&GLFW_MOD_CONTROL){
    switch(key){
      case GLFW_KEY_C:{char v=0x03;pty.write(&v,1);return;}
      case GLFW_KEY_D:{char v=0x04;pty.write(&v,1);return;}
      case GLFW_KEY_L:{char v=0x0C;pty.write(&v,1);return;}
      case GLFW_KEY_Z:{char v=0x1A;pty.write(&v,1);return;}
      case GLFW_KEY_EQUAL:case GLFW_KEY_KP_ADD:    reload_font_size(FONT_SIZE+2);return;
      case GLFW_KEY_MINUS:case GLFW_KEY_KP_SUBTRACT:reload_font_size(FONT_SIZE-2);return;
      case GLFW_KEY_0:case GLFW_KEY_KP_0:           reload_font_size(18);return;
      default:break;
    }
  }
  if(mods&GLFW_MOD_SUPER){
    switch(key){
      case GLFW_KEY_EQUAL:case GLFW_KEY_KP_ADD:    reload_font_size(FONT_SIZE+2);return;
      case GLFW_KEY_MINUS:case GLFW_KEY_KP_SUBTRACT:reload_font_size(FONT_SIZE-2);return;
      case GLFW_KEY_0:case GLFW_KEY_KP_0:           reload_font_size(18);return;
      default:break;
    }
  }
  const char *seq=nullptr;
  switch(key){
    case GLFW_KEY_ENTER:    {const char v[]="\r";   pty.write(v,1);return;}
    case GLFW_KEY_BACKSPACE:{const char v[]="\x7f"; pty.write(v,1);return;}
    case GLFW_KEY_TAB:      {const char v[]="\t";   pty.write(v,1);return;}
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
  if(seq) pty.write(seq,strlen(seq));
}

static void char_callback(GLFWwindow*,unsigned int cp){
  if(cp<0x80){char c=(char)cp;pty.write(&c,1);}
}

static void scroll_callback(GLFWwindow*,double,double yoff){
  static double accum=0.0; accum+=yoff;
  int max_offset;
  {std::lock_guard<std::mutex> lk(g_lock);max_offset=(int)g_term.scrollback.size();}
  if(max_offset==0){accum=0;return;}
  int step=(int)accum; if(step==0) return; accum-=step;
  {std::lock_guard<std::mutex> lk(g_lock);g_scroll_offset=std::clamp(g_scroll_offset-step,0,max_offset);}
  g_dirty=true;
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(){
  if(!glfwInit()) return -1;
  glfwWindowHint(GLFW_DECORATED,GLFW_TRUE);
  GLFWwindow *win=glfwCreateWindow(WINDOW_WIDTH,WINDOW_HEIGHT,WINDOW_TITLE.c_str(),nullptr,nullptr);
  if(!win) return -1;
  glfwMakeContextCurrent(win);
  if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

  int fbW,fbH; glfwGetFramebufferSize(win,&fbW,&fbH);
  g_fbWidth=fbW; g_fbHeight=fbH;
  glViewport(0,0,fbW,fbH);
  glfwSetFramebufferSizeCallback(win,framebuffer_size_callback);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(0.05f,0.05f,0.05f,1.f);

  // ── Terminal shader (required) ───────────────────────────────────────────
  g_termProgram = loadShaders("shader.vert","shader.frag");
  if(!g_termProgram){ std::cerr<<"Failed to load terminal shaders\n"; return -1; }

  // ── Background shader (optional — falls back to clear colour) ────────────
  g_bgProgram = loadShaders("bg.vert","bg.frag");
  if(g_bgProgram){
    g_bgTimeLoc = glGetUniformLocation(g_bgProgram,"uTime");
    g_bgResLoc  = glGetUniformLocation(g_bgProgram,"uResolution");

    static const float bgV[] = {
      -1.f,  1.f,  0.f,0.f,
       1.f,  1.f,  1.f,0.f,
       1.f, -1.f,  1.f,1.f,
      -1.f,  1.f,  0.f,0.f,
       1.f, -1.f,  1.f,1.f,
      -1.f, -1.f,  0.f,1.f,
    };
    glGenVertexArrays(1,&g_bgVAO);
    glGenBuffers(1,&g_bgVBO);
    glBindVertexArray(g_bgVAO);
      glBindBuffer(GL_ARRAY_BUFFER,g_bgVBO);
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
  if(FT_Init_FreeType(&g_ft)){std::cerr<<"FT init failed\n";return -1;}
  if(FT_New_Face(g_ft,"JetBrainsMono-Medium.ttf",0,&g_face)){std::cerr<<"Regular font missing\n";return -1;}
  FT_New_Face(g_ft,"JetBrainsMono-Bold.ttf",      0,&g_bold_face);
  FT_New_Face(g_ft,"JetBrainsMono-Italic.ttf",    0,&g_italic_face);
  FT_New_Face(g_ft,"JetBrainsMono-BoldItalic.ttf",0,&g_bold_italic_face);

  auto setSize=[](FT_Face f){if(f)FT_Set_Pixel_Sizes(f,0,(FT_UInt)FONT_SIZE);};
  setSize(g_face);setSize(g_bold_face);setSize(g_italic_face);setSize(g_bold_italic_face);
  load_glyphs();

  glGenTextures(4,g_atlasTex);
  upload_atlases();

  {std::lock_guard<std::mutex> lk(g_lock);grid_resize_locked();}

  // ── Terminal shader uniforms ──────────────────────────────────────────────
  glUseProgram(g_termProgram);
  glUniform1i(glGetUniformLocation(g_termProgram,"atlasRegular"),   0);
  glUniform1i(glGetUniformLocation(g_termProgram,"atlasBold"),      1);
  glUniform1i(glGetUniformLocation(g_termProgram,"atlasItalic"),    2);
  glUniform1i(glGetUniformLocation(g_termProgram,"atlasBoldItalic"),3);
  glUseProgram(0);

  // ── Terminal VAO (12 floats per vertex) ──────────────────────────────────
  const GLsizei STRIDE = 12*sizeof(float);
  glGenVertexArrays(1,&g_termVAO);
  glGenBuffers(1,&g_termVBO);
  glBindVertexArray(g_termVAO);
    glBindBuffer(GL_ARRAY_BUFFER,g_termVBO);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,STRIDE,(void*)0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,STRIDE,(void*)(2*sizeof(float)));
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,STRIDE,(void*)(4*sizeof(float)));
    glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,STRIDE,(void*)(7*sizeof(float)));
    glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,STRIDE,(void*)(10*sizeof(float)));
    glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,STRIDE,(void*)(11*sizeof(float)));
    for(int i=0;i<6;++i) glEnableVertexAttribArray(i);
    build_terminal_vertices(g_verts,fbW,fbH);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(g_verts.size()*sizeof(float)),g_verts.data(),GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER,0);
  glBindVertexArray(0);

  glfwSetKeyCallback(win,   key_callback);
  glfwSetCharCallback(win,  char_callback);
  glfwSetScrollCallback(win,scroll_callback);
  pty.setReadCallback(read_callback);
  pty.spawn(g_term.cols,g_term.rows);

  static bool lastBlink=false;

  while(!glfwWindowShouldClose(win)){
    if(g_dirty.exchange(false)){
      build_terminal_vertices(g_verts,g_fbWidth,g_fbHeight);
      upload_vbo();
    }
    bool blink=cursor_visible();
    if(blink!=lastBlink){g_dirty=true;lastBlink=blink;}

    float now=(float)glfwGetTime();
    glClear(GL_COLOR_BUFFER_BIT);

    // 1) Background
    if(g_bgProgram){
      glUseProgram(g_bgProgram);
      if(g_bgTimeLoc>=0) glUniform1f(g_bgTimeLoc,now);
      if(g_bgResLoc>=0)  glUniform2f(g_bgResLoc,(float)g_fbWidth,(float)g_fbHeight);
      glBindVertexArray(g_bgVAO);
      glDrawArrays(GL_TRIANGLES,0,6);
      glBindVertexArray(0);
    }

    // 2) Terminal
    glUseProgram(g_termProgram);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g_atlasTex[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,g_atlasTex[1]);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,g_atlasTex[2]);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D,g_atlasTex[3]);
    glBindVertexArray(g_termVAO);
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)(g_verts.size()/12));
    glBindVertexArray(0);

    glfwSwapBuffers(win);
    glfwPollEvents();
  }

  pty.stop();
  if(g_face)            FT_Done_Face(g_face);
  if(g_bold_face)       FT_Done_Face(g_bold_face);
  if(g_italic_face)     FT_Done_Face(g_italic_face);
  if(g_bold_italic_face)FT_Done_Face(g_bold_italic_face);
  FT_Done_FreeType(g_ft);
  glfwTerminate();
  return 0;
}