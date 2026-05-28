// compress.h — Simple compression utilities
// Adapted from zepto8 by Sam Hocevar (WTFPL)

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace z8
{

std::vector<uint8_t> compress(std::vector<uint8_t> &input);
std::string encode49(std::vector<uint8_t> const &v);

} // namespace z8
