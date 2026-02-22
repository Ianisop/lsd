#pragma once
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <glm/vec3.hpp>
#include <filesystem>
#include <toml++/toml.hpp>

namespace LSD::Config
{
extern std::string loaded_config;// holds the whole config file for tokenization
extern glm::vec3 font_color;
extern int font_size;
extern int max_fps;
const std::string TERMINAL_CONFIG_KEY = "[terminal]";
const std::string UI_CONFIG_KEY = "[face]";

bool parse(const std::string &config);
bool reload();
}// namespace LSD::Config