#pragma once
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <glm/common.hpp>

// Forward declare what we need from LSD namespace
namespace LSD
{
namespace Types
{
  struct TermState;// Forward declaration
}

// Declare the global variables and functions we need
extern Types::TermState terminal_state;
void gridScrollUpLocked();
}// namespace LSD

#include "parse/ansi_parser.h"
#include "types/lsd_types.h"

namespace LSD::CsiParser
{
static void process_csi_locked(const std::string &params, char fb)
{
  auto parse = [&](int def) -> std::vector<int> {
    std::vector<int> v;
    std::stringstream ss(params);
    std::string t;
    while (std::getline(ss, t, ';')) { v.push_back(t.empty() || t == "0" ? def : std::stoi(t)); }
    if (v.empty()) v.push_back(def);
    return v;
  };

  auto P = [&](int i, int def) -> int {
    auto v = parse(def);
    return i < (int)v.size() ? v[i] : def;
  };

  if (!params.empty() && params[0] == '?') return;

  // Now these should be found
  int &col = terminal_state.cur_col;
  int &row = terminal_state.cur_row;
  const int C = terminal_state.cols;
  const int R = terminal_state.rows;
  auto &grid = terminal_state.grid;

  row = glm::clamp(row, 0, R - 1);
  col = glm::clamp(col, 0, C - 1);

  switch (fb)
    {
    case 'm':
      LSD::AnsiParser::apply_sgr(params);
      break;
    case 'A':
      row = std::max(0, row - P(0, 1));
      break;
    case 'B':
      row = std::min(R - 1, row + P(0, 1));
      break;
    case 'C':
      col = std::min(C - 1, col + P(0, 1));
      break;
    case 'D':
      col = std::max(0, col - P(0, 1));
      break;
    case 'E':
      row = std::min(R - 1, row + P(0, 1));
      col = 0;
      break;
    case 'F':
      row = std::max(0, row - P(0, 1));
      col = 0;
      break;
    case 'G':
      col = glm::clamp(P(0, 1) - 1, 0, C - 1);
      break;
    case 'd':
      row = glm::clamp(P(0, 1) - 1, 0, R - 1);
      break;
    case 'H':
      case 'f': {
        auto v = parse(1);
        row = glm::clamp((v.size() > 0 ? v[0] : 1) - 1, 0, R - 1);
        col = glm::clamp((v.size() > 1 ? v[1] : 1) - 1, 0, C - 1);
        break;
      }
      case 'J': {
        int n = P(0, 0);
        if (n == 0)
          {
            for (int c = col; c < C; ++c) grid[row][c] = LSD::Types::Cell{};
            for (int r = row + 1; r < R; ++r) grid[r].assign(C, LSD::Types::Cell{});
          }
        else if (n == 1)
          {
            for (int r = 0; r < row; ++r) grid[r].assign(C, LSD::Types::Cell{});
            for (int c = 0; c <= col && c < C; ++c) grid[row][c] = LSD::Types::Cell{};
          }
        else if (n == 2 || n == 3)
          {
            for (auto &r : grid) r.assign(C, LSD::Types::Cell{});
            row = 0;
            col = 0;
          }
        break;
      }
      case 'K': {
        int n = P(0, 0);
        if (n == 0)
          {
            for (int c = col; c < C; ++c) grid[row][c] = LSD::Types::Cell{};
          }
        else if (n == 1)
          {
            for (int c = 0; c <= col && c < C; ++c) grid[row][c] = LSD::Types::Cell{};
          }
        else if (n == 2) { grid[row].assign(C, LSD::Types::Cell{}); }
        break;
      }
    case 'L':
      for (int i = 0; i < P(0, 1); ++i)
        {
          grid.insert(grid.begin() + row, std::vector<LSD::Types::Cell>(C));
          if ((int)grid.size() > R) grid.resize(R);
        }
      break;
    case 'M':
      for (int i = 0; i < P(0, 1); ++i)
        {
          if (row < (int)grid.size()) grid.erase(grid.begin() + row);
          grid.emplace_back(C);
        }
      break;
    case 'S':
      for (int i = 0; i < P(0, 1); ++i) gridScrollUpLocked();// Now should be found
      break;
    case 'T':
      for (int i = 0; i < P(0, 1); ++i)
        {
          grid.pop_back();
          grid.insert(grid.begin(), std::vector<LSD::Types::Cell>(C));
        }
      break;
    case 'X':
      for (int i = 0; i < P(0, 1) && col + i < C; ++i) { grid[row][col + i] = LSD::Types::Cell{}; }
      break;
    case 'h':
    case 'l':
    case 'r':
      break;
    default:
      break;
    }
}
}// namespace LSD::CsiParser