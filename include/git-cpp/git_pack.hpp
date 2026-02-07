#pragma once

#include <string>

#include "git-cpp/git_object_store.hpp"
#include "git-cpp/git_result.hpp"

namespace gitcpp {

class GitPackProcessor {
 public:
  [[nodiscard]] GitExpected<std::string> UnpackAndStore(
      const std::string& upload_pack_response, const std::string& head_hash,
      const GitObjectStore& object_store) const;
};

}  // namespace gitcpp
