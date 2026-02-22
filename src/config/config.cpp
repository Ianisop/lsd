#include "config.h"
#include <toml++/impl/parser.hpp>

namespace LSD::Config
{
std::string loaded_config;// holds the whole config file for tokenization
glm::vec3 font_color;
int font_size;
int max_fps;

bool load_config(const std::string &path)
{
  if (std::filesystem::exists(path))
    {
      std::ifstream file(path);
      return true;
    }
  printf("WARNING: config file doesn't exist, defaulting to hardcoded values | %s\n", path.c_str());
  return false;
}

bool parse(const std::string &config)
{
  auto loaded = toml::parse_file(config);

  return false;
}
}// namespace LSD::Config