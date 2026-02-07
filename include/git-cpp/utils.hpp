#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <zlib.h>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

GitExpected<std::string> ZlibDecompressString(const std::string& compressed_data);
GitExpected<std::string> ZlibCompressString(const std::string& input_string);
GitStatus ZlibCompressFile(FILE* input, FILE* output);
GitStatus ZlibDecompressFile(FILE* input, FILE* output);

std::string ComputeSha1(std::string_view data);
std::string HashToHex(std::string_view hash_bytes);

}  // namespace gitcpp
