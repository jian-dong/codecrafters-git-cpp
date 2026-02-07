#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

class GitRepository {
 public:
  static GitExpected<std::filesystem::path> FindGitRoot(
      std::filesystem::path start_path = std::filesystem::current_path());
  static GitExpected<GitRepository> Open(
      std::filesystem::path start_path = std::filesystem::current_path());
  static GitExpected<GitRepository> Init(const std::filesystem::path& repo_path);

  explicit GitRepository(std::filesystem::path git_dir);

  [[nodiscard]] const std::filesystem::path& git_dir() const noexcept;
  [[nodiscard]] GitExpected<std::string> ReadObject(
      const std::string& object_hash) const;
  [[nodiscard]] GitExpected<std::string> HashObject(
      const std::filesystem::path& file_path) const;
  [[nodiscard]] GitExpected<std::string> WriteTree(
      const std::filesystem::path& directory) const;
  [[nodiscard]] GitExpected<std::vector<std::string>> ListTreeNames(
      const std::string& tree_hash) const;
  [[nodiscard]] GitExpected<std::string> CommitTree(
      const std::string& tree_hash, const std::string& parent_hash,
      const std::string& commit_message) const;

 private:
  std::filesystem::path git_dir_;
};

}  // namespace gitcpp
