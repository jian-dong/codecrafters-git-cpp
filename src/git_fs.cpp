#include "git-cpp/git_fs.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace gitcpp {

GitExpected<std::string> ReadBinaryFile(const std::filesystem::path& file_path) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input.is_open()) {
    return UnexpectedError(GitErrorCode::kIoError, "Failed to open file",
                           "read binary file", file_path.string());
  }

  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  if (input.bad()) {
    return UnexpectedError(GitErrorCode::kIoError, "Failed to read file",
                           "read binary file", file_path.string());
  }
  return content;
}

GitStatus WriteBinaryFile(const std::filesystem::path& file_path,
                          std::string_view data) {
  std::error_code create_ec;
  if (file_path.has_parent_path()) {
    std::filesystem::create_directories(file_path.parent_path(), create_ec);
    if (create_ec) {
      return UnexpectedSystemError(
          GitErrorCode::kIoError, "write binary file: create directories",
          file_path.parent_path().string(), create_ec, "Failed to create directory");
    }
  }

  std::ofstream output(file_path, std::ios::binary);
  if (!output.is_open()) {
    return UnexpectedError(GitErrorCode::kIoError,
                           "Failed to open file for writing", "write binary file",
                           file_path.string());
  }
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!output.good()) {
    return UnexpectedError(GitErrorCode::kIoError, "Failed to write file",
                           "write binary file", file_path.string());
  }
  return {};
}

}  // namespace gitcpp
