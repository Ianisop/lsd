#pragma once
#include <iostream>
#include <glm/vec3.hpp>
#include "types/lsd_types.h"
#include <sstream>


namespace LSD::AnsiParser
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

static AnsiState g_ansi_state;

static glm::vec3 ansi_to_rgb(int n)
{
  switch (n)
    {
    case 30:
      return { 0, 0, 0 };
    case 31:
      return { .8f, 0, 0 };
    case 32:
      return { 0, .8f, 0 };
    case 33:
      return { .8f, .8f, 0 };
    case 34:
      return { 0, 0, .8f };
    case 35:
      return { .8f, 0, .8f };
    case 36:
      return { 0, .8f, .8f };
    case 37:
      return { .8f, .8f, .8f };
    case 90:
      return { .5f, .5f, .5f };
    case 91:
      return { 1, 0, 0 };
    case 92:
      return { 0, 1, 0 };
    case 93:
      return { 1, 1, 0 };
    case 94:
      return { .3f, .3f, 1 };
    case 95:
      return { 1, 0, 1 };
    case 96:
      return { 0, 1, 1 };
    case 97:
      return { 1, 1, 1 };
    default:
      return { 1, 1, 1 };
    }
}

static void apply_sgr(const std::string &params)
{
  if (params.empty() || params == "0")
    {
      g_ansi_state.fg = { 1, 1, 1 };
      g_ansi_state.bg = { 0, 0, 0 };
      g_ansi_state.bold = false;
      g_ansi_state.italic = false;
      return;
    }
  std::stringstream ss(params);
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
        case 0:
          g_ansi_state.fg = { 0, .8f, .6f };
          g_ansi_state.bg = { 0, 0, 0 };
          g_ansi_state.bold = false;
          g_ansi_state.italic = false;
          break;
        case 1:
          g_ansi_state.bold = true;
          break;
        case 3:
          g_ansi_state.italic = true;
          break;
        case 22:
          g_ansi_state.bold = false;
          break;
        case 23:
          g_ansi_state.italic = false;
          break;
        case 39:
          g_ansi_state.fg = { 0, .8f, .6f };
          break;
        case 49:
          g_ansi_state.bg = { 0, 0, 0 };
          break;
        default:
          if ((n >= 30 && n <= 37) || (n >= 90 && n <= 97))
            g_ansi_state.fg = ansi_to_rgb(n);
          else if (n >= 40 && n <= 47)
            g_ansi_state.bg = ansi_to_rgb(n - 10);
          else if (n >= 100 && n <= 107)
            g_ansi_state.bg = ansi_to_rgb(n - 70);
          break;
        }
    }
}
}// namespace LSD::AnsiParser