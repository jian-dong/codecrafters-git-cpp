#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git-cpp/git_result.hpp"

namespace gitcpp {

struct GitTreeEntry {
  std::string mode;
  std::string name;
  std::string hash_hex;
};

GitExpected<std::string> HexToBytes(std::string_view hex_value);
GitExpected<std::vector<GitTreeEntry>> ParseTreeEntries(std::string_view tree_body);
GitExpected<std::string> ExtractCommitTreeHash(std::string_view commit_body);

}  // namespace gitcpp
