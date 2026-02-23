#pragma once
#include <iostream>
#include <glm/vec3.hpp>
#include <sstream>
#include "config/config.h"
#include "types/lsd_types.h"

namespace LSD::AnsiParser
{
using namespace LSD::Types;
enum class EscState
{
  Normal,
  Esc,
  CSI,
  OSC
};

static AnsiState next_ansi_state;

static glm::vec3 ansi_to_rgb(int n)
{
  switch (n)
    {
    case 30:
      next_ansi_state.fg_override = true;

      return { 0, 0, 0 };
    case 31:
      next_ansi_state.fg_override = true;

      return { 0.8f, 0, 0 };
    case 32:
      next_ansi_state.fg_override = true;

      return { 0, 0.8f, 0 };
    case 33:
      next_ansi_state.fg_override = true;

      return { 0.8f, 0.8f, 0 };
    case 34:
      next_ansi_state.fg_override = true;

      return { 0, 0, 0.8f };
    case 35:
      next_ansi_state.fg_override = true;

      return { 0.8f, 0, 0.8f };
    case 36:
      next_ansi_state.fg_override = true;

      return { 0, 0.8f, 0.8f };
    case 37:
      next_ansi_state.fg_override = true;

      return { 0.8f, 0.8f, 0.8f };
    case 90:
      next_ansi_state.fg_override = true;

      return { 0.5f, 0.5f, 0.5f };
    case 91:
      next_ansi_state.fg_override = true;

      return { 1, 0, 0 };
    case 92:
      next_ansi_state.fg_override = true;

      return { 0, 1, 0 };
    case 93:
      next_ansi_state.fg_override = true;

      return { 1, 1, 0 };
    case 94:
      next_ansi_state.fg_override = true;

      return { 0.3f, 0.3f, 1 };
    case 95:
      next_ansi_state.fg_override = true;

      return { 1, 0, 1 };
    case 96:
      next_ansi_state.fg_override = true;

      return { 0, 1, 1 };
    case 97:
      next_ansi_state.fg_override = true;
      return { 1, 1, 1 };
    default:
      return LSD::Config::font_color;// fallback to default
    }
}

static void apply_sgr(const std::string &params)
{
  std::cout << "SGR param: " << params << "\n";

  // Empty params = reset (same as 0)
  std::string p = params.empty() ? "0" : params;

  std::stringstream ss(p);
  std::string tok;

  while (std::getline(ss, tok, ';'))
    {
      if (tok.empty()) tok = "0";

      int n = 0;
      try
        {
          n = std::stoi(tok);
        }
      catch (...)
        {
          continue;
        }

      switch (n)
        {
        // ===== RESET =====
        case 0:
          next_ansi_state.fg = LSD::Config::font_color;
          next_ansi_state.bg = { 0.f, 0.f, 0.f };
          next_ansi_state.bold = false;
          next_ansi_state.italic = false;
          break;

        // ===== STYLE FLAGS =====
        case 1:
          next_ansi_state.bold = true;
          break;
        case 3:
          next_ansi_state.italic = true;
          break;
        case 22:
          next_ansi_state.bold = false;
          break;
        case 23:
          next_ansi_state.italic = false;
          break;

        case 39:// reset fg
          next_ansi_state.fg = LSD::Config::font_color;
          break;

        case 49:// reset bg
          next_ansi_state.bg = { 0.f, 0.f, 0.f };
          break;

        default:
          // ===== FOREGROUND COLORS =====
          if ((n >= 30 && n <= 37) || (n >= 90 && n <= 97)) { next_ansi_state.fg = ansi_to_rgb(n); }
          // ===== BACKGROUND COLORS =====
          else if ((n >= 40 && n <= 47) || (n >= 100 && n <= 107))
            {
              // background codes are fg + 10
              next_ansi_state.bg = ansi_to_rgb(n - 10);
            }
          break;
        }
    }
}

}// namespace LSD::AnsiParser