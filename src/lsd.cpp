#include <chrono>
#include <ctime>
#include <glad/glad.h>
#include "lsd.h"
#include "clip.h"
#include "config/config.h"
#include "lsd_pty.h"
#include "parse/csi_parser.h"
#include "parse/osc_parser.h"
#include "types/lsd_types.h"
#include <GLFW/glfw3.h>
#include <bits/stdc++.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include "config/config.h"
#include <vector>

#ifdef RELEASE_INSTALL_PATH
std::string ASSET_PATH = RELEASE_INSTALL_PATH;
#else
std::string ASSET_PATH = DEBUG_INSTALL_PATH;
#endif

const char *get_config_path()
{
  static std::string path = [] {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) return std::string(xdg) + "/lsd/config.toml";

    if (const char *home = std::getenv("HOME")) return std::string(home) + "/.config/lsd/config.toml";

    return std::string("./config/lsd/config.toml");
  }();

  return path.c_str();
}

namespace LSD
{
std::string WINDOW_TITLE = "lsd";
int FONT_SIZE = LSD::Config::font_size;

const char *CONFIG_PATH = get_config_path();

std::mutex lock;
LSD::Types::TerminalState *current_terminal_state;
std::atomic<bool> dirt_flag{ false };
int scroll_offset = 0;
PTY *current_pty;

LSD::Types::TerminalState terminal_states[3];
std::string current_terminal_label_data;

int g_fbWidth = LSD::WINDOW_WIDTH;
int g_fbHeight = LSD::WINDOW_HEIGHT;
int glyph_width = 0;
int glyph_height = 0;

double delta_time = 0;
const double target_frame_time = 1.0 / LSD::MAX_FPS;
double frame_start_time = 0.0;
double frame_end_time = 0.0;

std::vector<LSD::Types::CopiedChar> clipboard;

GLuint g_terminal_program = 0;
GLuint g_terminal_vao = 0;
GLuint g_terminal_VBO = 0;
std::vector<float> g_vertices;// vertex buffer for everything

GLuint g_background_program = 0;
GLuint g_background_VAO = 0;
GLuint g_background_VBO = 0;
GLint g_background_time_loc = -1;
GLint g_background_res_Loc = -1;

FT_Library font_library;
FT_Face font_normal_face = nullptr;
FT_Face font_bold_face = nullptr;
FT_Face font_italic_face = nullptr;
FT_Face font_bold_italic_face = nullptr;

GLuint atlas_tex[4] = { 0, 0, 0, 0 };

std::map<char, LSD::Types::Glyph> glyphs[4];

unsigned char atlas_px[4][LSD::ATLAS_WIDTH * LSD::ATLAS_HEIGHT];

// Shader helpers
static std::string loadFile(const std::string &path)
{
  std::ifstream f(path);
  if (!f)
    {
      std::cerr << "Cannot open: " << path << "\n";
      return "";
    }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static GLuint compileShader(GLenum type, const char *src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok)
    {
      char log[512];
      glGetShaderInfoLog(s, 512, nullptr, log);
      std::cerr << log << "\n";
    }
  return s;
}

// Returns 0 if either file is missing/fails - caller must check
static GLuint loadShaders(const std::string &vpath, const std::string &fpath)
{
  std::string vsrc = loadFile(vpath), fsrc = loadFile(fpath);
  if (vsrc.empty() || fsrc.empty()) return 0;
  GLuint vs = compileShader(GL_VERTEX_SHADER, vsrc.c_str());
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsrc.c_str());
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok)
    {
      char log[512];
      glGetProgramInfoLog(p, 512, nullptr, log);
      std::cerr << log << "\n";
    }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return p;
}

// Atlas
static void buildAtlas(FT_Face face, std::map<char, LSD::Types::Glyph> &out, unsigned char *px)
{
  if (!face) return;
  memset(px, 0, LSD::ATLAS_WIDTH * LSD::ATLAS_HEIGHT);
  out.clear();
  int x = 0, y = 0, rowH = 0;
  for (unsigned char c = 32; c < 127; ++c)
    {
      if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) continue;
      int w = (int)face->glyph->bitmap.width, h = (int)face->glyph->bitmap.rows;
      if (x + w >= LSD::ATLAS_WIDTH)
        {
          x = 0;
          y += rowH;
          rowH = 0;
        }
      if (y + h >= LSD::ATLAS_HEIGHT)
        {
          std::cerr << "Atlas overflow\n";
          break;
        }
      for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) px[(y + j) * LSD::ATLAS_WIDTH + (x + i)] = face->glyph->bitmap.buffer[j * face->glyph->bitmap.pitch + i];
      LSD::Types::Glyph g;
      g.u0 = float(x) / LSD::ATLAS_WIDTH;
      g.u1 = float(x + w) / LSD::ATLAS_WIDTH;
      g.v0 = float(y) / LSD::ATLAS_HEIGHT;
      g.v1 = float(y + h) / LSD::ATLAS_HEIGHT;
      if (g.u1 > 1.0f || g.v1 > 1.0f) std::cerr << "Glyph UV outside atlas\n";

      g.width = w;
      g.height = h;
      g.bearingX = face->glyph->bitmap_left;
      g.bearingY = face->glyph->bitmap_top;
      g.advance = face->glyph->advance.x >> 6;
      out[(char)c] = g;
      x += w;
      rowH = std::max(rowH, h);
    }
}

void loadGylphs()
{
  FT_Face faces[4] = { LSD::font_normal_face, LSD::font_bold_face, LSD::font_italic_face, LSD::font_bold_italic_face };
  for (int i = 0; i < 4; ++i) buildAtlas(faces[i], LSD::glyphs[i], LSD::atlas_px[i]);
  LSD::glyph_width = 0;
  LSD::glyph_height = 0;
  for (auto &[c, g] : LSD::glyphs[0])
    {
      LSD::glyph_width = std::max(LSD::glyph_width, g.advance);
      LSD::glyph_height = std::max(LSD::glyph_height, g.height);
    }
  std::cout << "Font " << LSD::FONT_SIZE << "px  cell " << LSD::glyph_width << "x" << LSD::glyph_height << "\n";
}

void uploadAtlases()
{
  for (int i = 0; i < 4; ++i)
    {
      glBindTexture(GL_TEXTURE_2D, LSD::atlas_tex[i]);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, LSD::ATLAS_WIDTH, LSD::ATLAS_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, LSD::atlas_px[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
}

// Grid helpers (call with g_lock held)
void gridResizeLocked()
{
  if (LSD::glyph_width == 0 || LSD::glyph_height == 0) return;

  // Get the actual line height from the font (includes spacing)
  int lineHeight = LSD::font_normal_face->size->metrics.height >> 6;

  // Calculate available rows for terminal content (excluding status bar)
  // Reserve one line height for status bar at the bottom
  int available_height = LSD::g_fbHeight - lineHeight;
  int new_cols = std::max(1, LSD::g_fbWidth / LSD::glyph_width);
  int new_rows = std::max(1, available_height / lineHeight);

  bool empty = LSD::current_terminal_state->grid.empty();
  if (!empty && new_cols == LSD::current_terminal_state->cols && new_rows == LSD::current_terminal_state->rows) return;

  auto old = LSD::current_terminal_state->grid;
  int oc = LSD::current_terminal_state->cols, or_ = LSD::current_terminal_state->rows;

  LSD::current_terminal_state->cols = new_cols;
  LSD::current_terminal_state->rows = new_rows;
  LSD::current_terminal_state->grid.assign(new_rows, std::vector<LSD::Types::Cell>(new_cols));

  if (!empty)
    {
      for (int r = 0; r < std::min(or_, new_rows); ++r)
        {
          for (int c = 0; c < std::min(oc, new_cols); ++c) { LSD::current_terminal_state->grid[r][c] = old[r][c]; }
        }
    }

  // Ensure cursor is within bounds
  LSD::current_terminal_state->cur_col = std::clamp(LSD::current_terminal_state->cur_col, 0, new_cols - 1);
  LSD::current_terminal_state->cur_row = std::clamp(LSD::current_terminal_state->cur_row, 0, new_rows - 1);

  printf("FB Height: %d, Line Height: %d, Available: %d, Rows: %d\n", LSD::g_fbHeight, lineHeight, available_height, new_rows);

  current_terminal_state->status_bar.assign(current_terminal_state->cols, Types::Cell{});
}

void gridScrollUpLocked()
{
  if (LSD::current_terminal_state->grid.empty()) return;

  // Move top line to scrollback
  LSD::current_terminal_state->scrollback.push_back(LSD::current_terminal_state->grid.front());
  if ((int)LSD::current_terminal_state->scrollback.size() > LSD::Types::TerminalState::MAX_SCROLLBACK) LSD::current_terminal_state->scrollback.pop_front();

  // Shift all rows up
  for (int r = 0; r < LSD::current_terminal_state->rows - 1; r++) { LSD::current_terminal_state->grid[r] = std::move(LSD::current_terminal_state->grid[r + 1]); }

  // Clear the last row
  LSD::current_terminal_state->grid[LSD::current_terminal_state->rows - 1].assign(LSD::current_terminal_state->cols, LSD::Types::Cell{});

  // Cursor stays where it is (should be at bottom row when this is called from newline)
  // printf("SCROLL UP: Cursor at row %d\n", LSD::terminal_state.cur_row);
}

void gridNewlineLocked()
{
  LSD::current_terminal_state->cur_col = 0;

  // If we're at the bottom row, scroll the content up
  if (LSD::current_terminal_state->cur_row == LSD::current_terminal_state->rows - 1)
    {
      // Scroll the entire grid up
      LSD::current_terminal_state->scrollback.push_back(LSD::current_terminal_state->grid.front());
      if ((int)LSD::current_terminal_state->scrollback.size() > LSD::Types::TerminalState::MAX_SCROLLBACK) LSD::current_terminal_state->scrollback.pop_front();

      // Shift all rows up
      for (int r = 0; r < LSD::current_terminal_state->rows - 1; r++) { LSD::current_terminal_state->grid[r] = std::move(LSD::current_terminal_state->grid[r + 1]); }

      // Clear the last row
      LSD::current_terminal_state->grid[LSD::current_terminal_state->rows - 1].assign(LSD::current_terminal_state->cols, LSD::Types::Cell{});

      // Cursor stays at bottom row
      LSD::current_terminal_state->cur_row = LSD::current_terminal_state->rows - 1;
    }
  else
    {
      // Just move to next row
      LSD::current_terminal_state->cur_row++;
    }

  // printf("NEWLINE: ROWS: %i | CURSOR: %i\n", LSD::terminal_state.rows, LSD::terminal_state.cur_row);
}

void gridPutLocked(char c)
{
  int &col = LSD::current_terminal_state->cur_col;
  int &row = LSD::current_terminal_state->cur_row;
  const int COLS = LSD::current_terminal_state->cols;
  const int ROWS = LSD::current_terminal_state->rows;

  // Handle control characters first
  switch (c)
    {
    case '\n':
      LSD::gridNewlineLocked();
      return;
    case '\r':
      col = 0;
      return;
    case '\t':
      col = std::min(((col / 8) + 1) * 8, COLS - 1);
      return;
    case '\b':
      if (col > 0) --col;
      return;
    default:
      break;
    }

  // Skip other control characters
  if (c < 0x20) return;

  // Handle line wrapping
  if (col >= COLS)
    {
      col = 0;
      // Move to next row (this will handle scrolling if needed)
      if (row == ROWS - 1)
        {
          // At bottom row - need to scroll
          LSD::gridScrollUpLocked();
          // row stays at ROWS - 1
        }
      else
        {
          row++;
        }
    }

  // Validate indices
  if (row < 0 || row >= ROWS) return;
  if (col < 0 || col >= COLS) return;

  // Place the character
  LSD::current_terminal_state->grid[row][col] = LSD::Types::Cell{ c, LSD::AnsiParser::next_ansi_state, LSD::AnsiParser::next_ansi_state.fg, LSD::AnsiParser::next_ansi_state.bg, false };
  // Move to next column
  ++col;
}

using time_point = std::chrono::system_clock::time_point;
std::string serializeTimePoint(const time_point &time, const std::string &format)
{
  std::time_t tt = std::chrono::system_clock::to_time_t(time);
  std::tm tm = *std::localtime(&tt);
  // std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by default.
  std::stringstream ss;
  ss << std::put_time(&tm, format.c_str());
  return ss.str();
}

void writeSegment(std::vector<LSD::Types::Cell> &bar, int start, int width, const std::string &text,
  int align)// 0=left,1=center,2=right
{
  if (width <= 0) return;

  int text_len = (int)text.size();
  int offset = 0;

  if (align == 1)// center
    offset = (width - text_len) / 2;
  else if (align == 2)// right
    offset = width - text_len;

  offset = std::max(0, offset);

  for (int i = 0; i < width; ++i)
    {
      int bar_index = start + i;
      if (bar_index >= (int)bar.size()) break;

      int text_index = i - offset;

      if (text_index >= 0 && text_index < text_len)
        bar[bar_index].ch = text[text_index];
      else
        bar[bar_index].ch = ' ';
    }
}

void fillStatusBar()
{
  auto &bar = current_terminal_state->status_bar;

  int W = bar.size();
  int splits = 3;
  int seg_w = W / splits;

  std::string left = serializeTimePoint(std::chrono::system_clock::now(), "%Y-%d-%m %H:%M:%S");

  std::string *middle = &current_terminal_label_data;

  std::string right = std::to_string(delta_time) + " ms";

  writeSegment(bar, 0, seg_w, left, 0);// left aligned
  writeSegment(bar, seg_w, seg_w, *middle, 1);// centered
  writeSegment(bar, seg_w * 2, W - seg_w * 2, right, 2);// right aligned

  for (auto &c : bar)
    {
      c.bg = { 1, 1, 1 };
      c.fg = { 0, 0, 0 };
    }
}

// PTY callback
void read_callback(const char *msg, size_t size)
{
  std::lock_guard<std::mutex> lk(LSD::lock);
  for (size_t i = 0; i < size; ++i)
    {
      unsigned char byte = (unsigned char)msg[i];
      switch (LSD::AnsiParser::next_ansi_state.state)
        {
        case LSD::Types::EscState::Normal:
          if (byte == 0x1B)
            {
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Esc;
              LSD::AnsiParser::next_ansi_state.param_buf.clear();
            }
          else
            LSD::gridPutLocked((char)byte);
          break;
        case LSD::Types::EscState::Esc:
          LSD::AnsiParser::next_ansi_state.param_buf.clear();
          if (byte == '[')
            LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::CSI;
          else if (byte == ']')
            LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::OSC;
          else if (byte == 'c')
            {
              for (auto &r : LSD::current_terminal_state->grid) r.assign(LSD::current_terminal_state->cols, LSD::Types::Cell{});
              LSD::current_terminal_state->cur_row = 0;
              LSD::current_terminal_state->cur_col = 0;
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Normal;
            }
          else if (byte == 'M')
            {
              if (LSD::current_terminal_state->cur_row > 0) --LSD::current_terminal_state->cur_row;
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Normal;
            }
          else
            LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Normal;
          break;
        case LSD::Types::EscState::CSI:
          if (byte >= 0x40 && byte <= 0x7E)
            {
              LSD::CsiParser::process_csi_locked(LSD::AnsiParser::next_ansi_state.param_buf, (char)byte);
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Normal;
              LSD::AnsiParser::next_ansi_state.param_buf.clear();
            }
          else
            LSD::AnsiParser::next_ansi_state.param_buf += (char)byte;
          break;
        case LSD::Types::EscState::OSC:
          if (byte == 0x07)
            {
              LSD::OscParser::process_osc(LSD::AnsiParser::next_ansi_state.param_buf);
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Normal;
              LSD::AnsiParser::next_ansi_state.param_buf.clear();
            }
          else if (byte == 0x1B)
            {
              LSD::OscParser::process_osc(LSD::AnsiParser::next_ansi_state.param_buf);
              LSD::AnsiParser::next_ansi_state.state = LSD::Types::EscState::Esc;
              LSD::AnsiParser::next_ansi_state.param_buf.clear();
            }
          else
            LSD::AnsiParser::next_ansi_state.param_buf += (char)byte;
          break;
        }
    }
  LSD::scroll_offset = 0;
  LSD::dirt_flag = true;
}

// Vertex builder
// Vertex layout (12 floats): x y  u v  fg.rgb  bg.rgb  isGlyph  styleIndex
// Style index:  0=regular 1=bold 2=italic 3=bolditalic  96=cellbg  98=cursorglyph  99=cursorblock
void buildTerminalVertices(std::vector<float> &verts, int W, int H)
{
  verts.clear();
  if (!LSD::font_normal_face || LSD::glyph_width == 0 || LSD::glyph_height == 0) return;

  int ascent = LSD::font_normal_face->size->metrics.ascender >> 6;
  int lineHeight = LSD::font_normal_face->size->metrics.height >> 6;
  float cellW = (float)LSD::glyph_width;
  float cellH = (float)lineHeight;

  // Calculate the total height used by terminal rows
  int terminalHeight = LSD::current_terminal_state->rows * cellH;

  // Status bar starts right after the terminal content
  float statusY0 = terminalHeight;
  float statusY1 = H;

  auto ndcX = [W](float px) { return px / W * 2.f - 1.f; };
  auto ndcY = [H](float py) { return 1.f - py / H * 2.f; };

  auto emit = [&](float vx, float vy, float vu, float vv, glm::vec3 fg, glm::vec3 bg, float isG, float sI) { verts.insert(verts.end(), { vx, vy, vu, vv, fg.r, fg.g, fg.b, bg.r, bg.g, bg.b, isG, sI }); };

  // Snapshot under lock
  std::vector<std::vector<LSD::Types::Cell>> snap;
  int cursorRow = -1;
  int cursorCol = 0;
  LSD::Types::Cell cursorCell;

  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    int rows = LSD::current_terminal_state->rows;
    int offset = std::clamp(LSD::scroll_offset, 0, (int)LSD::current_terminal_state->scrollback.size());

    if (offset == 0)
      {
        snap = LSD::current_terminal_state->grid;
        cursorRow = LSD::current_terminal_state->cur_row;
        cursorCol = LSD::current_terminal_state->cur_col;
      }
    else
      {
        int sbSize = (int)LSD::current_terminal_state->scrollback.size();
        int sbStart = std::max(0, sbSize - offset);
        int sbRows = std::min(offset, rows);

        snap.reserve(rows);

        for (int i = sbStart; i < sbStart + sbRows && i < sbSize; ++i) { snap.push_back(LSD::current_terminal_state->scrollback[i]); }

        for (int i = 0; i < rows - sbRows; ++i)
          {
            if (i < (int)LSD::current_terminal_state->grid.size()) { snap.push_back(LSD::current_terminal_state->grid[i]); }
          }

        cursorRow = -1;
        cursorCol = -1;
      }

    if (cursorRow >= 0 && cursorRow < (int)snap.size() && cursorCol >= 0 && cursorCol < (int)snap[cursorRow].size()) { cursorCell = snap[cursorRow][cursorCol]; }
  }

  int visibleRows = (int)snap.size();

  // Render terminal cells
  for (int row = 0; row < visibleRows; ++row)
    {
      for (int col = 0; col < (int)snap[row].size(); ++col)
        {
          const LSD::Types::Cell &cell = snap[row][col];
          glm::vec3 fg = cell.ansi_state.fg_override ? cell.ansi_state.fg : LSD::Config::font_color;
          glm::vec3 bg = cell.selected ? glm::vec3(1, 1, 1) : cell.ansi_state.bg;
          // std::cout << "Char: " << cell.ch << " FG: " << LSD::AnsiParser::next_ansi_state.fg.r << "," << LSD::AnsiParser::next_ansi_state.fg.g << "," << LSD::AnsiParser::next_ansi_state.fg.b << "\n";
          float x0 = col * cellW;
          float y0 = row * cellH;
          float x1 = x0 + cellW;
          float y1 = y0 + cellH;

          // Don't render if beyond terminal area
          if (y1 > terminalHeight) continue;

          float nx0 = ndcX(x0), nx1 = ndcX(x1);
          float ny0 = ndcY(y0), ny1 = ndcY(y1);
          glm::vec3 dummy{ 0, 0, 0 };

          // Cell background
          if ((bg.r + bg.g + bg.b) > 0.01f)
            {
              emit(nx0, ny0, 0, 0, bg, dummy, 0, 96.f);
              emit(nx1, ny0, 0, 0, bg, dummy, 0, 96.f);
              emit(nx1, ny1, 0, 0, bg, dummy, 0, 96.f);
              emit(nx0, ny0, 0, 0, bg, dummy, 0, 96.f);
              emit(nx1, ny1, 0, 0, bg, dummy, 0, 96.f);
              emit(nx0, ny1, 0, 0, bg, dummy, 0, 96.f);
            }

          char ch = cell.ch;
          if (ch == ' ' || ch == '\0') continue;

          int gi = 0;
          if (cell.ansi_state.bold && cell.ansi_state.italic && LSD::font_bold_italic_face)
            gi = 3;
          else if (cell.ansi_state.bold && LSD::font_bold_face)
            gi = 1;
          else if (cell.ansi_state.italic && LSD::font_italic_face)
            gi = 2;

          auto it = LSD::glyphs[gi].find(ch);
          if (it == LSD::glyphs[gi].end())
            {
              it = LSD::glyphs[0].find(ch);
              gi = 0;
            }
          if (it == LSD::glyphs[0].end()) continue;

          const LSD::Types::Glyph &g = it->second;
          float gx0 = x0 + g.bearingX;
          float gy0 = y0 + ascent - g.bearingY;
          float gx1 = gx0 + g.width;
          float gy1 = gy0 + g.height;

          float ngx0 = ndcX(gx0), ngx1 = ndcX(gx1);
          float ngy0 = ndcY(gy0), ngy1 = ndcY(gy1);

          float sI = (float)gi;

          emit(ngx0, ngy0, g.u0, g.v0, fg, bg, 1, sI);
          emit(ngx1, ngy0, g.u1, g.v0, fg, bg, 1, sI);
          emit(ngx1, ngy1, g.u1, g.v1, fg, bg, 1, sI);
          emit(ngx0, ngy0, g.u0, g.v0, fg, bg, 1, sI);
          emit(ngx1, ngy1, g.u1, g.v1, fg, bg, 1, sI);
          emit(ngx0, ngy1, g.u0, g.v1, fg, bg, 1, sI);
        }
    }

  // Render status bar at bottom
  std::vector<LSD::Types::Cell> statusBar;
  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    statusBar = LSD::current_terminal_state->status_bar;
  }

  for (int col = 0; col < (int)statusBar.size() && col < LSD::current_terminal_state->cols; ++col)
    {
      const LSD::Types::Cell &cell = statusBar[col];
      glm::vec3 fg = cell.fg;
      glm::vec3 bg = cell.bg;

      float x0 = col * cellW;
      float x1 = x0 + cellW;

      float nx0 = ndcX(x0), nx1 = ndcX(x1);
      float ny0 = ndcY(statusY0), ny1 = ndcY(statusY1);
      glm::vec3 dummy{ 0, 0, 0 };

      // Status bar background
      if ((bg.r + bg.g + bg.b) > 0.01f)
        {
          emit(nx0, ny0, 0, 0, bg, dummy, 0, 96.f);
          emit(nx1, ny0, 0, 0, bg, dummy, 0, 96.f);
          emit(nx1, ny1, 0, 0, bg, dummy, 0, 96.f);
          emit(nx0, ny0, 0, 0, bg, dummy, 0, 96.f);
          emit(nx1, ny1, 0, 0, bg, dummy, 0, 96.f);
          emit(nx0, ny1, 0, 0, bg, dummy, 0, 96.f);
        }

      char ch = cell.ch;
      if (ch == ' ' || ch == '\0') continue;

      int gi = 0;
      if (cell.ansi_state.bold && cell.ansi_state.italic && LSD::font_bold_italic_face)
        gi = 3;
      else if (cell.ansi_state.bold && LSD::font_bold_face)
        gi = 1;
      else if (cell.ansi_state.italic && LSD::font_italic_face)
        gi = 2;

      auto it = LSD::glyphs[gi].find(ch);
      if (it == LSD::glyphs[gi].end())
        {
          it = LSD::glyphs[0].find(ch);
          gi = 0;
        }
      if (it == LSD::glyphs[0].end()) continue;

      const LSD::Types::Glyph &g = it->second;
      float gx0 = x0 + g.bearingX;
      float gy0 = statusY0 + ascent - g.bearingY;
      float gx1 = gx0 + g.width;
      float gy1 = gy0 + g.height;

      float ngx0 = ndcX(gx0), ngx1 = ndcX(gx1);
      float ngy0 = ndcY(gy0), ngy1 = ndcY(gy1);
      float sI = (float)gi;

      emit(ngx0, ngy0, g.u0, g.v0, fg, bg, 1, sI);
      emit(ngx1, ngy0, g.u1, g.v0, fg, bg, 1, sI);
      emit(ngx1, ngy1, g.u1, g.v1, fg, bg, 1, sI);
      emit(ngx0, ngy0, g.u0, g.v0, fg, bg, 1, sI);
      emit(ngx1, ngy1, g.u1, g.v1, fg, bg, 1, sI);
      emit(ngx0, ngy1, g.u0, g.v1, fg, bg, 1, sI);
    }

  // Cursor (only in live view)
  if (LSD::cursorVisible() && LSD::scroll_offset == 0 && cursorRow >= 0 && cursorRow < LSD::current_terminal_state->rows)
    {
      float x0 = cursorCol * cellW;
      float y0 = cursorRow * cellH;
      float x1 = x0 + cellW;
      float y1 = y0 + cellH;

      // Don't render cursor in status bar area
      if (y1 <= terminalHeight)
        {
          float nx0 = ndcX(x0), nx1 = ndcX(x1);
          float ny0 = ndcY(y0), ny1 = ndcY(y1);

          glm::vec3 white{ 1, 1, 1 }, dummy{ 0, 0, 0 };

          // Draw solid white cursor block
          emit(nx0, ny0, 0, 0, white, dummy, 0, 99.f);
          emit(nx1, ny0, 0, 0, white, dummy, 0, 99.f);
          emit(nx1, ny1, 0, 0, white, dummy, 0, 99.f);
          emit(nx0, ny0, 0, 0, white, dummy, 0, 99.f);
          emit(nx1, ny1, 0, 0, white, dummy, 0, 99.f);
          emit(nx0, ny1, 0, 0, white, dummy, 0, 99.f);

          // Cursor glyph
          char ch = cursorCell.ch;
          if (ch != ' ' && ch != '\0')
            {
              int gi = 0;
              if (cursorCell.ansi_state.bold && cursorCell.ansi_state.italic && LSD::font_bold_italic_face)
                gi = 3;
              else if (cursorCell.ansi_state.bold && LSD::font_bold_face)
                gi = 1;
              else if (cursorCell.ansi_state.italic && LSD::font_italic_face)
                gi = 2;

              auto it = LSD::glyphs[gi].find(ch);
              if (it == LSD::glyphs[gi].end())
                {
                  it = LSD::glyphs[0].find(ch);
                  gi = 0;
                }
              if (it != LSD::glyphs[gi].end())
                {
                  const LSD::Types::Glyph &g = it->second;

                  int ascentG = 0;
                  switch (gi)
                    {
                    case 0:
                      if (LSD::font_normal_face) ascentG = LSD::font_normal_face->size->metrics.ascender >> 6;
                      break;
                    case 1:
                      if (LSD::font_bold_face) ascentG = LSD::font_bold_face->size->metrics.ascender >> 6;
                      break;
                    case 2:
                      if (LSD::font_italic_face) ascentG = LSD::font_italic_face->size->metrics.ascender >> 6;
                      break;
                    case 3:
                      if (LSD::font_bold_italic_face) ascentG = LSD::font_bold_italic_face->size->metrics.ascender >> 6;
                      break;
                    }

                  float gx0 = x0 + g.bearingX;
                  float gy0 = y0 + ascentG - g.bearingY;
                  float gx1 = gx0 + g.width;
                  float gy1 = gy0 + g.height;

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
}

void uploadVbo()
{
  glBindBuffer(GL_ARRAY_BUFFER, LSD::g_terminal_VBO);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(LSD::g_vertices.size() * sizeof(float)), LSD::g_vertices.data(), GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void switchCurrentTerminalState(const int &index)
{

  current_terminal_state = &terminal_states[index];
  current_pty = &current_terminal_state->pty;

  if (!current_terminal_state->pty.is_started)
    {
      current_pty->spawn();
      LSD::current_pty->setReadCallback(LSD::read_callback);// TODO: find way to unsubscribe maybe
    }

  gridResizeLocked();
  current_terminal_label_data = "[";
  current_terminal_label_data += std::to_string(index);
  current_terminal_label_data += "]";
}

void reloadFontSize(int sz)
{
  if (sz < LSD::FONT_SIZE_MIN || sz > LSD::FONT_SIZE_MAX) return;
  LSD::FONT_SIZE = sz;
  auto ss = [](FT_Face f) {
    if (f) FT_Set_Pixel_Sizes(f, 0, (FT_UInt)LSD::FONT_SIZE);
  };
  ss(LSD::font_normal_face);
  ss(LSD::font_bold_face);
  ss(LSD::font_italic_face);
  ss(LSD::font_bold_italic_face);
  loadGylphs();
  uploadAtlases();
  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    LSD::gridResizeLocked();
  }
  LSD::current_pty->resize(LSD::current_terminal_state->cols, LSD::current_terminal_state->rows);
  LSD::dirt_flag = true;
}

void copySelectedText()
{
  int x = current_terminal_state->cur_col - 1;
  int y = current_terminal_state->cur_row;
  auto &cell = current_terminal_state->grid[y][x];
  cell.selected = true;

  clipboard.insert(clipboard.begin(), { cell.ch, { y, x } });
}

void copySelectedTextIntoClipboard()
{
  std::string s;
  for (auto &copied : clipboard)
    {
      s += copied.ch;
      current_terminal_state->grid[copied.old_grid_position.x][copied.old_grid_position.y].selected = false;
    }
  clip::set_text(s);
}

void remove_text_from_selected()
{
  if (clipboard.empty()) return;

  auto pos = clipboard.front().old_grid_position;
  current_terminal_state->grid[pos.x][pos.y].selected = false;
  clipboard.erase(clipboard.begin());
}

// In framebuffer_size_callback, update the PTY resize:
void framebufferSizeCallback(GLFWwindow *, int w, int h)
{
  if (w <= 0 || h <= 0) return;
  glViewport(0, 0, w, h);
  LSD::g_fbWidth = w;
  LSD::g_fbHeight = h;
  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    LSD::gridResizeLocked();
  }
  // PTY should get the terminal content size, not including status bar
  LSD::current_pty->resize(LSD::current_terminal_state->cols, LSD::current_terminal_state->rows);
  LSD::dirt_flag = true;
}

static void keyCallback(GLFWwindow *, int key, int, int action, int mods)
{
  if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

  if (LSD::scroll_offset != 0)
    {
      LSD::scroll_offset = 0;
      LSD::dirt_flag = true;
    }
  if ((mods & GLFW_MOD_SHIFT) && (mods & GLFW_MOD_CONTROL))
    {
      switch (key)
        {
          // copy to clipboard and revert cells
          case GLFW_KEY_C: {
            copySelectedTextIntoClipboard();
            return;
          }
          // TODO: make this work
          case GLFW_KEY_V: {
            std::string paste;
            clip::get_text(paste);
            const char *begin = "\x1b[200~";
            const char *end = "\x1b[201~";

            LSD::current_pty->write(begin, 6);
            LSD::current_pty->write(paste.data(), paste.size());
            LSD::current_pty->write(end, 6);
          }
          break;
        }
    }

  // select via shift and arrow keys
  if (mods & GLFW_MOD_SHIFT)
    {
      switch (key)
        {
          case GLFW_KEY_LEFT: {
            copySelectedText();
            break;
          }
          case GLFW_KEY_RIGHT: {
            remove_text_from_selected();
            break;
          }
        }
    }

  // this gets filled with cases that i meet in tui apps i use, e.g nano's ultra specific method of exiting
  if (mods & GLFW_MOD_CONTROL)
    {
      switch (key)
        {
          case GLFW_KEY_C: {
            char v = 0x03;
            LSD::current_pty->write(&v, 1);
            return;
          }
          case GLFW_KEY_D: {
            char v = 0x04;
            LSD::current_pty->write(&v, 1);
            return;
          }
          case GLFW_KEY_L: {
            char v = 0x0C;
            LSD::current_pty->write(&v, 1);
            return;
          }
          case GLFW_KEY_Z: {
            char v = 0x1A;
            LSD::current_pty->write(&v, 1);
            return;
          }
          case GLFW_KEY_X: {
            char *v = "\x18";
            LSD::current_pty->write(v, 1);
            return;
          }
          case GLFW_KEY_T: {
            char v = 0x14;
            LSD::current_pty->write(&v, 1);
            return;
          }
        case GLFW_KEY_EQUAL:
        case GLFW_KEY_KP_ADD:
          reloadFontSize(LSD::FONT_SIZE + 2);
          return;
        case GLFW_KEY_MINUS:
        case GLFW_KEY_KP_SUBTRACT:
          reloadFontSize(LSD::FONT_SIZE - 2);
          return;
        case GLFW_KEY_0:
        case GLFW_KEY_KP_0:
          reloadFontSize(18);
          return;
        case GLFW_KEY_1:
          switchCurrentTerminalState(0);
          return;
        case GLFW_KEY_2:
          switchCurrentTerminalState(1);
          return;
        case GLFW_KEY_3:
          switchCurrentTerminalState(2);
          return;
        }
    }
  if (mods & GLFW_MOD_SUPER)
    {
      switch (key)
        {
        case GLFW_KEY_EQUAL:
        case GLFW_KEY_KP_ADD:
          reloadFontSize(LSD::FONT_SIZE + 2);
          return;
        case GLFW_KEY_MINUS:
        case GLFW_KEY_KP_SUBTRACT:
          reloadFontSize(LSD::FONT_SIZE - 2);
          return;
        case GLFW_KEY_0:
        case GLFW_KEY_KP_0:
          reloadFontSize(18);
          return;
        default:
          break;
        }
    }
  const char *seq = nullptr;
  switch (key)
    {
      case GLFW_KEY_ENTER: {
        const char v[] = "\r";
        LSD::current_pty->write(v, 1);
        return;
      }
      case GLFW_KEY_BACKSPACE: {
        const char v[] = "\x7f";
        LSD::current_pty->write(v, 1);
        return;
      }
      case GLFW_KEY_TAB: {
        const char v[] = "\t";
        LSD::current_pty->write(v, 1);
        return;
      }
    case GLFW_KEY_UP:
      seq = "\033[A";
      break;
    case GLFW_KEY_DOWN:
      seq = "\033[B";
      break;
    case GLFW_KEY_RIGHT:
      seq = "\033[C";
      break;
    case GLFW_KEY_LEFT:
      seq = "\033[D";
      break;
    case GLFW_KEY_HOME:
      seq = "\033[H";
      break;
    case GLFW_KEY_END:
      seq = "\033[F";
      break;
    case GLFW_KEY_PAGE_UP:
      seq = "\033[5~";
      break;
    case GLFW_KEY_PAGE_DOWN:
      seq = "\033[6~";
      break;
    case GLFW_KEY_DELETE:
      seq = "\033[3~";
      break;
    case GLFW_KEY_INSERT:
      seq = "\033[2~";
      break;
    default:
      break;
    }
  if (seq) LSD::current_pty->write(seq, strlen(seq));
}

bool cursorVisible()
{
  // Simple implementation - cursor blinks every 0.5 seconds
  static double lastToggle = 0.0;
  static bool visible = true;
  double now = glfwGetTime();

  if (now - lastToggle > 0.5)
    {
      visible = !visible;
      lastToggle = now;
    }
  return visible;
}

static void charCallback(GLFWwindow *, unsigned int cp)
{
  if (cp < 0x80)
    {
      char c = (char)cp;
      LSD::current_pty->write(&c, 1);
    }
}

static void scrollCallback(GLFWwindow *, double, double yoff)
{
  static double accum = 0.0;
  accum += yoff;
  int max_offset;
  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    max_offset = (int)LSD::current_terminal_state->scrollback.size();
  }
  if (max_offset == 0)
    {
      accum = 0;
      return;
    }
  int step = (int)accum;
  if (step == 0) return;
  accum -= step;
  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    LSD::scroll_offset = std::clamp(LSD::scroll_offset - step, 0, max_offset);
  }
  LSD::dirt_flag = true;
}

void handleDeltaTime()
{

  double frame_end = glfwGetTime();

  double frame_time = frame_end - frame_start_time;

  double remaining = LSD::target_frame_time - frame_time;

  if (remaining > 0.0) { std::this_thread::sleep_for(std::chrono::duration<double>(remaining)); }

  // FINAL timestamp AFTER sleep
  double frame_final = glfwGetTime();
  LSD::delta_time = frame_final - frame_start_time;

  // printf("%d\n", (int)(1.0 / LSD::delta_time));
}

}// namespace LSD
int main()
{
  if (!glfwInit()) return -1;
  glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
  GLFWwindow *win = glfwCreateWindow(LSD::WINDOW_WIDTH, LSD::WINDOW_HEIGHT, LSD::WINDOW_TITLE.c_str(), nullptr, nullptr);
  if (!win) return -1;
  glfwMakeContextCurrent(win);
  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
  LSD::Config::load_or_make_config(get_config_path());
  LSD::Config::parse(get_config_path());

  int fbW, fbH;
  glfwGetFramebufferSize(win, &fbW, &fbH);
  LSD::g_fbWidth = fbW;
  LSD::g_fbHeight = fbH;
  glViewport(0, 0, fbW, fbH);
  glfwSetFramebufferSizeCallback(win, LSD::framebufferSizeCallback);
  glfwSwapInterval(0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(0.05f, 0.05f, 0.05f, 1.f);

  LSD::current_terminal_state = &LSD::terminal_states[0];
  LSD::current_pty = &LSD::current_terminal_state->pty;
  LSD::current_pty->setReadCallback(LSD::read_callback);// TODO: find way to unsubscribe maybe

  // Terminal shader (required)
  LSD::g_terminal_program = LSD::loadShaders(ASSET_PATH + "shaders/shader.vert", ASSET_PATH + "shaders/shader.frag");
  if (!LSD::g_terminal_program)
    {
      std::cerr << "Failed to load terminal shaders\n";
      return -1;
    }

  // Background shader falls back to clear color
  LSD::g_background_program = LSD::loadShaders(ASSET_PATH + "shaders/bg.vert", ASSET_PATH + "shaders/bg.frag");
  if (LSD::g_background_program)
    {
      LSD::g_background_time_loc = glGetUniformLocation(LSD::g_background_program, "uTime");
      LSD::g_background_res_Loc = glGetUniformLocation(LSD::g_background_program, "uResolution");

      static const float bgV[] = {
        -1.f,
        1.f,
        0.f,
        0.f,
        1.f,
        1.f,
        1.f,
        0.f,
        1.f,
        -1.f,
        1.f,
        1.f,
        -1.f,
        1.f,
        0.f,
        0.f,
        1.f,
        -1.f,
        1.f,
        1.f,
        -1.f,
        -1.f,
        0.f,
        1.f,
      };
      glGenVertexArrays(1, &LSD::g_background_VAO);
      glGenBuffers(1, &LSD::g_background_VBO);
      glBindVertexArray(LSD::g_background_VAO);
      glBindBuffer(GL_ARRAY_BUFFER, LSD::g_background_VBO);
      glBufferData(GL_ARRAY_BUFFER, sizeof(bgV), bgV, GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindVertexArray(0);
      std::cout << "Background shader loaded ok\n";
    }
  else
    {
      std::cerr << "bg.vert/bg.frag not found — using clear colour\n";
    }

  // ── Fonts ────────────────────────────────────────────────────────────────
  if (FT_Init_FreeType(&LSD::font_library))
    {
      std::cerr << "FT init failed\n";
      return -1;
    }
  if (FT_New_Face(LSD::font_library, (ASSET_PATH + "fonts/JetBrainsMono-Medium.ttf").c_str(), 0, &LSD::font_normal_face))
    {
      std::cerr << "Regular font missing\n";
      return -1;
    }
  FT_New_Face(LSD::font_library, (ASSET_PATH + "fonts/JetBrainsMono-Bold.ttf").c_str(), 0, &LSD::font_bold_face);
  FT_New_Face(LSD::font_library, (ASSET_PATH + "fonts/JetBrainsMono-Italic.ttf").c_str(), 0, &LSD::font_italic_face);
  FT_New_Face(LSD::font_library, (ASSET_PATH + "fonts/JetBrainsMono-BoldItalic.ttf").c_str(), 0, &LSD::font_bold_italic_face);

  auto setSize = [](FT_Face f) {
    if (f) FT_Set_Pixel_Sizes(f, 0, (FT_UInt)LSD::FONT_SIZE);
  };
  setSize(LSD::font_normal_face);
  setSize(LSD::font_bold_face);
  setSize(LSD::font_italic_face);
  setSize(LSD::font_bold_italic_face);
  LSD::loadGylphs();

  glGenTextures(4, LSD::atlas_tex);
  LSD::uploadAtlases();

  {
    std::lock_guard<std::mutex> lk(LSD::lock);
    LSD::gridResizeLocked();
  }

  // Terminal shader uniforms
  glUseProgram(LSD::g_terminal_program);
  glUniform1i(glGetUniformLocation(LSD::g_terminal_program, "atlasRegular"), 0);
  glUniform1i(glGetUniformLocation(LSD::g_terminal_program, "atlasBold"), 1);
  glUniform1i(glGetUniformLocation(LSD::g_terminal_program, "atlasItalic"), 2);
  glUniform1i(glGetUniformLocation(LSD::g_terminal_program, "atlasBoldItalic"), 3);
  glUseProgram(0);

  // Terminal VAO (12 floats per vertex)
  const GLsizei STRIDE = 12 * sizeof(float);
  glGenVertexArrays(1, &LSD::g_terminal_vao);
  glGenBuffers(1, &LSD::g_terminal_VBO);
  glBindVertexArray(LSD::g_terminal_vao);
  glBindBuffer(GL_ARRAY_BUFFER, LSD::g_terminal_VBO);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, STRIDE, (void *)0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, STRIDE, (void *)(2 * sizeof(float)));
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, STRIDE, (void *)(4 * sizeof(float)));
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, STRIDE, (void *)(7 * sizeof(float)));
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, STRIDE, (void *)(10 * sizeof(float)));
  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, STRIDE, (void *)(11 * sizeof(float)));
  for (int i = 0; i < 6; ++i) glEnableVertexAttribArray(i);
  LSD::buildTerminalVertices(LSD::g_vertices, fbW, fbH);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(LSD::g_vertices.size() * sizeof(float)), LSD::g_vertices.data(), GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  glfwSetKeyCallback(win, LSD::keyCallback);
  glfwSetCharCallback(win, LSD::charCallback);
  glfwSetScrollCallback(win, LSD::scrollCallback);

  LSD::current_pty->spawn(LSD::current_terminal_state->cols, LSD::current_terminal_state->rows);

  static bool lastBlink = false;

  while (!glfwWindowShouldClose(win))
    {
      LSD::frame_start_time = glfwGetTime();
      LSD::fillStatusBar();
      if (LSD::dirt_flag.exchange(false))
        {
          LSD::buildTerminalVertices(LSD::g_vertices, LSD::g_fbWidth, LSD::g_fbHeight);
          LSD::uploadVbo();
        }
      bool blink = LSD::cursorVisible();
      if (blink != lastBlink)
        {
          LSD::dirt_flag = true;
          lastBlink = blink;
        }

      float now = (float)glfwGetTime();
      glClear(GL_COLOR_BUFFER_BIT);

      // 1) Background
      if (LSD::g_background_program)
        {
          glUseProgram(LSD::g_background_program);
          if (LSD::g_background_time_loc >= 0) glUniform1f(LSD::g_background_time_loc, now);
          if (LSD::g_background_res_Loc >= 0) glUniform2f(LSD::g_background_res_Loc, (float)LSD::g_fbWidth, (float)LSD::g_fbHeight);
          glBindVertexArray(LSD::g_background_VAO);
          glDrawArrays(GL_TRIANGLES, 0, 6);
          glBindVertexArray(0);
        }

      // 2) Terminal
      glUseProgram(LSD::g_terminal_program);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, LSD::atlas_tex[0]);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, LSD::atlas_tex[1]);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, LSD::atlas_tex[2]);
      glActiveTexture(GL_TEXTURE3);
      glBindTexture(GL_TEXTURE_2D, LSD::atlas_tex[3]);
      glBindVertexArray(LSD::g_terminal_vao);
      glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(LSD::g_vertices.size() / 12));
      glBindVertexArray(0);

      glfwSwapBuffers(win);
      glfwPollEvents();

      LSD::handleDeltaTime();
    }

  LSD::current_pty->stop();
  if (LSD::font_normal_face) FT_Done_Face(LSD::font_normal_face);
  if (LSD::font_bold_face) FT_Done_Face(LSD::font_bold_face);
  if (LSD::font_italic_face) FT_Done_Face(LSD::font_italic_face);
  if (LSD::font_bold_italic_face) FT_Done_Face(LSD::font_bold_italic_face);
  FT_Done_FreeType(LSD::font_library);
  glfwTerminate();
  return 0;
}
