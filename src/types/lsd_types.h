#pragma once
#include <cstddef>
#include <deque>
#include <iostream>
#include <vector>
#include "glm/vec3.hpp"
#include "glm/vec2.hpp"
#include "lsd_pty.h"

namespace LSD::Types
{
enum class EscState
{
  Normal,
  Esc,
  CSI,
  OSC
};

struct AnsiState
{
  EscState state = EscState::Normal;
  std::string param_buf;
  glm::vec3 fg{ 0.f, 0.8f, 0.6f }, bg{ 0.f, 0.f, 0.f };
  bool bold = false, italic = false;
};

struct CopiedChar
{
  char ch = ' ';
  glm::vec2 old_grid_position{ 0, 0 };
};

struct Cell
{
  char ch = ' ';
  glm::vec3 fg{ 1.f, 1.f, 1.f };
  glm::vec3 bg{ 0.f, 0.f, 0.f };
  bool bold = false, italic = false, selected = false;
};

// Terminal state
class TerminalState
{
public:
  std::vector<std::vector<Cell>> grid;
  std::deque<std::vector<Cell>> scrollback;
  static constexpr int MAX_SCROLLBACK = 8000;
  std::vector<Cell> status_bar;
  PTY pty;

  int cols = 80, rows = 24, cur_col = 0, cur_row = 0;
};

struct Glyph
{
  float u0, v0, u1, v1;
  int width, height, bearingX, bearingY, advance;
};

}// namespace LSD::Types