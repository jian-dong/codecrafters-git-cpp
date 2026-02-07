#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

GitExpected<std::string> ReadBinaryFile(const std::filesystem::path& file_path);
GitStatus WriteBinaryFile(const std::filesystem::path& file_path,
                          std::string_view data);

}  // namespace gitcpp
