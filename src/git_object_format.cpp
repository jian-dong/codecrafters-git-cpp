#include "git-cpp/git_object_format.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "git-cpp/utils.hpp"

namespace gitcpp {
namespace {

constexpr std::size_t kHashHexLength = 40;
constexpr std::size_t kHashBinaryLength = 20;

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

bool IsHexHash(std::string_view hash) {
  if (hash.size() != kHashHexLength) {
    return false;
  }
  for (char ch : hash) {
    if (HexValue(ch) < 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

GitExpected<std::string> HexToBytes(std::string_view hex_value) {
  if ((hex_value.size() % 2) != 0) {
    return UnexpectedError(GitErrorCode::kInvalidArgument,
                           "Hex string has odd length", "hex to bytes");
  }

  std::string bytes;
  bytes.reserve(hex_value.size() / 2);
  for (std::size_t index = 0; index < hex_value.size(); index += 2) {
    const int high = HexValue(hex_value[index]);
    const int low = HexValue(hex_value[index + 1]);
    if (high < 0 || low < 0) {
      return UnexpectedError(GitErrorCode::kInvalidArgument, "Invalid hex string",
                             "hex to bytes");
    }
    const char byte = static_cast<char>((high << 4) | low);
    bytes.push_back(byte);
  }
  return bytes;
}

GitExpected<std::vector<GitTreeEntry>> ParseTreeEntries(std::string_view tree_body) {
  std::vector<GitTreeEntry> entries;
  std::size_t position = 0;
  while (position < tree_body.size()) {
    const std::size_t space_pos = tree_body.find(' ', position);
    if (space_pos == std::string::npos) {
      return UnexpectedError(GitErrorCode::kTreeFormatInvalid,
                             "Malformed tree object: missing mode separator",
                             "parse tree entries");
    }

    const std::size_t null_pos = tree_body.find('\0', space_pos + 1);
    if (null_pos == std::string::npos) {
      return UnexpectedError(GitErrorCode::kTreeFormatInvalid,
                             "Malformed tree object: missing name terminator",
                             "parse tree entries");
    }
    if (null_pos + 1 + kHashBinaryLength > tree_body.size()) {
      return UnexpectedError(GitErrorCode::kTreeFormatInvalid,
                             "Malformed tree object: truncated object hash",
                             "parse tree entries");
    }

    GitTreeEntry entry;
    entry.mode = std::string(tree_body.substr(position, space_pos - position));
    entry.name =
        std::string(tree_body.substr(space_pos + 1, null_pos - space_pos - 1));
    entry.hash_hex = HashToHex(tree_body.substr(null_pos + 1, kHashBinaryLength));
    entries.push_back(std::move(entry));
    position = null_pos + 1 + kHashBinaryLength;
  }

  return entries;
}

GitExpected<std::string> ExtractCommitTreeHash(std::string_view commit_body) {
  const std::size_t tree_pos = commit_body.find("tree ");
  if (tree_pos == std::string::npos || tree_pos + 5 + kHashHexLength > commit_body.size()) {
    return UnexpectedError(GitErrorCode::kCommitFormatInvalid,
                           "Invalid commit object: missing tree hash",
                           "extract commit tree hash");
  }

  const std::string tree_hash =
      std::string(commit_body.substr(tree_pos + 5, kHashHexLength));
  if (!IsHexHash(tree_hash)) {
    return UnexpectedError(GitErrorCode::kCommitFormatInvalid, "Invalid commit tree hash",
                           "extract commit tree hash");
  }
  return tree_hash;
}

}  // namespace gitcpp
