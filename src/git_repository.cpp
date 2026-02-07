#include "git-cpp/git_repository.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "git-cpp/git_fs.hpp"
#include "git-cpp/git_object_store.hpp"
#include "git-cpp/git_object_format.hpp"

namespace gitcpp {
namespace {

constexpr std::string_view kGitDirectoryName = ".git";

GitExpected<std::string> WriteTreeRecursive(const std::filesystem::path& directory,
                                            const GitObjectStore& object_store) {
  std::vector<GitTreeEntry> entries;
  std::error_code iteration_ec;
  std::filesystem::directory_iterator iterator(directory, iteration_ec);
  if (iteration_ec) {
    return UnexpectedSystemError(GitErrorCode::kIoError,
                                 "write tree: iterate directory",
                                 directory.string(), iteration_ec,
                                 "Failed to iterate directory");
  }

  for (const auto& entry : iterator) {
    const std::filesystem::path filename = entry.path().filename();
    if (filename == kGitDirectoryName) {
      continue;
    }

    if (entry.is_directory()) {
      auto tree_hash = WriteTreeRecursive(entry.path(), object_store);
      if (!tree_hash) {
        return UnexpectedError(tree_hash.error());
      }
      entries.push_back({"40000", filename.string(), *tree_hash});
    } else if (entry.is_regular_file()) {
      auto content = ReadBinaryFile(entry.path());
      if (!content) {
        return UnexpectedError(content.error());
      }
      auto blob_hash = object_store.StoreObject("blob", *content);
      if (!blob_hash) {
        return UnexpectedError(blob_hash.error());
      }
      entries.push_back({"100644", filename.string(), *blob_hash});
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const GitTreeEntry& lhs, const GitTreeEntry& rhs) {
              return lhs.name < rhs.name;
            });

  std::string tree_body;
  for (const GitTreeEntry& entry : entries) {
    auto hash_bytes = HexToBytes(entry.hash_hex);
    if (!hash_bytes) {
      return UnexpectedError(hash_bytes.error());
    }
    tree_body += entry.mode;
    tree_body.push_back(' ');
    tree_body += entry.name;
    tree_body.push_back('\0');
    tree_body += *hash_bytes;
  }

  return object_store.StoreObject("tree", tree_body);
}

}  // namespace

GitExpected<std::filesystem::path> GitRepository::FindGitRoot(
    std::filesystem::path start_path) {
  auto current_path = std::move(start_path);
  while (!current_path.empty()) {
    const std::filesystem::path candidate = current_path / kGitDirectoryName;
    std::error_code ec;
    const bool exists = std::filesystem::exists(candidate, ec);
    if (ec) {
      return UnexpectedSystemError(GitErrorCode::kIoError,
                                   "find git root: query path",
                                   candidate.string(), ec,
                                   "Failed to query path");
    }
    if (exists && std::filesystem::is_directory(candidate, ec)) {
      if (ec) {
        return UnexpectedSystemError(GitErrorCode::kIoError,
                                     "find git root: query directory",
                                     candidate.string(), ec,
                                     "Failed to query directory");
      }
      return candidate;
    }

    if (!current_path.has_parent_path()) {
      break;
    }
    const auto parent = current_path.parent_path();
    if (parent == current_path) {
      break;
    }
    current_path = parent;
  }
  return UnexpectedError(GitErrorCode::kRepositoryNotFound, "Not a git repository",
                         "find git root");
}

GitExpected<GitRepository> GitRepository::Open(std::filesystem::path start_path) {
  auto git_dir = FindGitRoot(std::move(start_path));
  if (!git_dir) {
    return UnexpectedError(git_dir.error());
  }
  return GitRepository(*git_dir);
}

GitExpected<GitRepository> GitRepository::Init(const std::filesystem::path& repo_path) {
  std::error_code ec;
  std::filesystem::create_directories(repo_path / ".git" / "objects", ec);
  if (ec) {
    return UnexpectedSystemError(
        GitErrorCode::kIoError, "init repository: create objects directory",
        (repo_path / ".git" / "objects").string(), ec,
        "Failed to create objects directory");
  }

  std::filesystem::create_directories(repo_path / ".git" / "refs" / "heads", ec);
  if (ec) {
    return UnexpectedSystemError(
        GitErrorCode::kIoError, "init repository: create refs directory",
        (repo_path / ".git" / "refs" / "heads").string(), ec,
        "Failed to create refs directory");
  }

  std::ofstream head_file(repo_path / ".git" / "HEAD", std::ios::binary);
  if (!head_file.is_open()) {
    return UnexpectedError(GitErrorCode::kIoError,
                           "Failed to open .git/HEAD for writing",
                           "init repository", (repo_path / ".git" / "HEAD").string());
  }
  head_file << "ref: refs/heads/main\n";
  if (!head_file.good()) {
    return UnexpectedError(GitErrorCode::kIoError, "Failed to write .git/HEAD",
                           "init repository", (repo_path / ".git" / "HEAD").string());
  }

  return GitRepository(repo_path / ".git");
}

GitRepository::GitRepository(std::filesystem::path git_dir)
    : git_dir_(std::move(git_dir)) {}

const std::filesystem::path& GitRepository::git_dir() const noexcept {
  return git_dir_;
}

GitExpected<std::string> GitRepository::ReadObject(
    const std::string& object_hash) const {
  GitObjectStore object_store(git_dir_);
  return object_store.ReadObjectBody(object_hash);
}

GitExpected<std::string> GitRepository::HashObject(
    const std::filesystem::path& file_path) const {
  GitObjectStore object_store(git_dir_);
  auto content = ReadBinaryFile(file_path);
  if (!content) {
    return UnexpectedError(content.error());
  }
  return object_store.StoreObject("blob", *content);
}

GitExpected<std::string> GitRepository::WriteTree(
    const std::filesystem::path& directory) const {
  GitObjectStore object_store(git_dir_);
  return WriteTreeRecursive(directory, object_store);
}

GitExpected<std::vector<std::string>> GitRepository::ListTreeNames(
    const std::string& tree_hash) const {
  GitObjectStore object_store(git_dir_);
  auto tree_body = object_store.ReadObjectBody(tree_hash);
  if (!tree_body) {
    return UnexpectedError(tree_body.error());
  }

  auto entries = ParseTreeEntries(*tree_body);
  if (!entries) {
    return UnexpectedError(entries.error());
  }

  std::vector<std::string> names;
  names.reserve(entries->size());
  for (const GitTreeEntry& entry : *entries) {
    names.push_back(entry.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

GitExpected<std::string> GitRepository::CommitTree(
    const std::string& tree_hash, const std::string& parent_hash,
    const std::string& commit_message) const {
  GitObjectStore object_store(git_dir_);

  auto tree_exists = object_store.ObjectExists(tree_hash);
  if (!tree_exists) {
    return UnexpectedError(tree_exists.error());
  }
  if (!*tree_exists) {
    return UnexpectedError(GitErrorCode::kTreeNotFound, "Tree object not found",
                           "commit tree", tree_hash);
  }

  if (!parent_hash.empty()) {
    auto parent_exists = object_store.ObjectExists(parent_hash);
    if (!parent_exists) {
      return UnexpectedError(parent_exists.error());
    }
    if (!*parent_exists) {
      return UnexpectedError(GitErrorCode::kCommitNotFound, "Parent commit not found",
                             "commit tree", parent_hash);
    }
  }

  const std::string identity = "John Doe <john.doe@gmail.com>";
  const auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  std::ostringstream commit_builder;
  commit_builder << "tree " << tree_hash << '\n';
  if (!parent_hash.empty()) {
    commit_builder << "parent " << parent_hash << '\n';
  }
  commit_builder << "author " << identity << ' ' << now << " -0800\n";
  commit_builder << "committer " << identity << ' ' << now << " -0800\n\n";
  commit_builder << commit_message << '\n';
  return object_store.StoreObject("commit", commit_builder.str());
}

}  // namespace gitcpp
