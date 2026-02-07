#pragma once

#include <filesystem>
#include <string>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

class GitRemoteClient {
 public:
  GitStatus Clone(const std::string& repo_url,
                  const std::filesystem::path& destination) const;
};

}  // namespace gitcpp
