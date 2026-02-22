#include "config.h"
#include "lsd.h"
#include <toml++/impl/parser.hpp>

namespace LSD::Config
{
std::string loaded_config;// holds the whole config file for tokenization
glm::vec3 font_color;
int font_size;
int max_fps;

bool load_or_make_config(const std::string &path)
{
  // TODO: check if directory exists too, should exist cos it gets created via install.sh
  // file exists
  if (std::filesystem::exists(path)) { return true; }

  return false;
}

bool parse(const std::string &config)
{
  auto table = toml::parse_file(config);
  auto font_size = table["font"]["size"].value_or(LSD::FONT_SIZE_DEFAULT);

  if (auto arr = table["font"]["color"].as_array())
    {
      if (arr->size() == 3)
        {
          LSD::Config::font_color.x = arr->at(0).value_or(1.0);
          LSD::Config::font_color.y = arr->at(1).value_or(1.0);
          LSD::Config::font_color.z = arr->at(2).value_or(1.0);
        }
    }
  LSD::FONT_SIZE = font_size;

  return false;
}
}// namespace LSD::Config