#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

class GitObjectStore {
 public:
  explicit GitObjectStore(std::filesystem::path git_dir);

  [[nodiscard]] const std::filesystem::path& git_dir() const noexcept;
  [[nodiscard]] GitExpected<bool> ObjectExists(std::string_view object_hash) const;
  [[nodiscard]] GitExpected<std::string> ReadRawObject(
      std::string_view object_hash) const;
  [[nodiscard]] GitExpected<std::string> ReadObjectBody(
      std::string_view object_hash) const;
  [[nodiscard]] GitExpected<std::string> StoreObject(std::string_view object_type,
                                                     std::string_view object_body) const;
  GitStatus StoreRawObject(std::string_view object_hash,
                           std::string_view raw_object) const;

 private:
  [[nodiscard]] GitExpected<std::filesystem::path> ObjectPath(
      std::string_view object_hash) const;
  [[nodiscard]] static GitStatus ValidateObjectHash(std::string_view object_hash);
  [[nodiscard]] static std::string BuildRawObject(std::string_view object_type,
                                                  std::string_view object_body);
  [[nodiscard]] static GitExpected<std::string> ExtractBody(std::string_view raw_object);

  std::filesystem::path git_dir_;
};

}  // namespace gitcpp
