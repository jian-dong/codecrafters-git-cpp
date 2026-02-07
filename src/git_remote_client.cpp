#include "git-cpp/git_remote_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "git-cpp/git_fs.hpp"
#include "git-cpp/git_object_store.hpp"
#include "git-cpp/git_object_format.hpp"
#include "git-cpp/git_pack.hpp"
#include "git-cpp/git_repository.hpp"

namespace gitcpp {
namespace {

constexpr std::size_t kHashHexLength = 40;

bool IsHexHash(std::string_view hash) {
  if (hash.size() != kHashHexLength) {
    return false;
  }
  for (char ch : hash) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

size_t WriteToString(void* received_data, size_t size, size_t count,
                     void* user_data) {
  const std::size_t total_size = size * count;
  auto* destination = static_cast<std::string*>(user_data);
  destination->append(static_cast<const char*>(received_data), total_size);
  return total_size;
}

std::optional<std::string> FindRefHash(std::string_view info_refs,
                                       std::string_view ref_name) {
  std::size_t position = 0;
  while ((position = info_refs.find(ref_name, position)) != std::string::npos) {
    if (position >= kHashHexLength + 1) {
      const std::string candidate =
          std::string(info_refs.substr(position - (kHashHexLength + 1), kHashHexLength));
      if (IsHexHash(candidate)) {
        return candidate;
      }
    }
    position += ref_name.size();
  }
  return std::nullopt;
}

GitExpected<std::string> SelectHeadHash(std::string_view info_refs) {
  for (const std::string_view preferred_ref : {"refs/heads/master", "refs/heads/main"}) {
    auto hash = FindRefHash(info_refs, preferred_ref);
    if (hash.has_value()) {
      return *hash;
    }
  }

  std::size_t position = 0;
  while ((position = info_refs.find("refs/heads/", position)) != std::string::npos) {
    if (position >= kHashHexLength + 1) {
      const std::string candidate =
          std::string(info_refs.substr(position - (kHashHexLength + 1), kHashHexLength));
      if (IsHexHash(candidate)) {
        return candidate;
      }
    }
    position += sizeof("refs/heads/") - 1;
  }
  return UnexpectedError(GitErrorCode::kRemoteRefNotFound,
                         "Failed to find remote branch hash from info/refs",
                         "select remote HEAD");
}

GitExpected<std::pair<std::string, std::string>> FetchRepositoryData(
    const std::string& repo_url) {
  using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
  using CurlHeaders = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

  CurlHandle curl_handle(curl_easy_init(), &curl_easy_cleanup);
  if (!curl_handle) {
    return UnexpectedError(GitErrorCode::kNetworkError, "Failed to initialize curl",
                           "fetch repository data");
  }

  std::string info_refs;
  curl_easy_setopt(curl_handle.get(), CURLOPT_URL,
                   (repo_url + "/info/refs?service=git-upload-pack").c_str());
  curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, &info_refs);
  curl_easy_setopt(curl_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl_handle.get(), CURLOPT_FAILONERROR, 1L);

  CURLcode code = curl_easy_perform(curl_handle.get());
  if (code != CURLE_OK) {
    return UnexpectedError(GitErrorCode::kNetworkError,
                           "Failed to fetch info/refs: " +
                               std::string(curl_easy_strerror(code)),
                           "fetch repository data", repo_url);
  }

  auto head_hash = SelectHeadHash(info_refs);
  if (!head_hash) {
    return UnexpectedError(head_hash.error());
  }

  curl_easy_reset(curl_handle.get());
  const std::string request_body = "0032want " + *head_hash + "\n00000009done\n";

  CurlHeaders headers(nullptr, &curl_slist_free_all);
  headers.reset(curl_slist_append(
      headers.release(), "Content-Type: application/x-git-upload-pack-request"));
  if (!headers) {
    return UnexpectedError(GitErrorCode::kInternalError,
                           "Failed to allocate curl headers",
                           "fetch repository data");
  }

  std::string upload_pack_response;
  curl_easy_setopt(curl_handle.get(), CURLOPT_URL, (repo_url + "/git-upload-pack").c_str());
  curl_easy_setopt(curl_handle.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl_handle.get(), CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl_handle.get(), CURLOPT_POSTFIELDSIZE, request_body.size());
  curl_easy_setopt(curl_handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, &upload_pack_response);
  curl_easy_setopt(curl_handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl_handle.get(), CURLOPT_FAILONERROR, 1L);

  code = curl_easy_perform(curl_handle.get());
  if (code != CURLE_OK) {
    return UnexpectedError(GitErrorCode::kNetworkError,
                           "Failed to fetch upload-pack stream: " +
                               std::string(curl_easy_strerror(code)),
                           "fetch repository data", repo_url);
  }

  return std::make_pair(upload_pack_response, *head_hash);
}

GitStatus RestoreTreeRecursive(const std::string& tree_hash,
                               const std::filesystem::path& output_dir,
                               const GitObjectStore& object_store) {
  auto tree_body = object_store.ReadObjectBody(tree_hash);
  if (!tree_body) {
    return UnexpectedError(tree_body.error());
  }
  auto entries = ParseTreeEntries(*tree_body);
  if (!entries) {
    return UnexpectedError(entries.error());
  }

  for (const GitTreeEntry& entry : *entries) {
    const std::filesystem::path target_path = output_dir / entry.name;
    if (entry.mode == "40000") {
      std::error_code ec;
      std::filesystem::create_directories(target_path, ec);
      if (ec) {
        return UnexpectedSystemError(
            GitErrorCode::kIoError, "restore tree: create directory",
            target_path.string(), ec, "Failed to create directory");
      }
      auto status = RestoreTreeRecursive(entry.hash_hex, target_path, object_store);
      if (!status) {
        return status;
      }
      continue;
    }

    auto blob_body = object_store.ReadObjectBody(entry.hash_hex);
    if (!blob_body) {
      return UnexpectedError(blob_body.error());
    }
    auto write_status = WriteBinaryFile(target_path, *blob_body);
    if (!write_status) {
      return write_status;
    }
  }

  return {};
}

}  // namespace

GitStatus GitRemoteClient::Clone(const std::string& repo_url,
                                 const std::filesystem::path& destination) const {
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    return UnexpectedSystemError(GitErrorCode::kIoError,
                                 "clone: create destination directory",
                                 destination.string(), ec,
                                 "Failed to create destination directory");
  }

  auto repository = GitRepository::Init(destination);
  if (!repository) {
    return UnexpectedError(repository.error());
  }
  GitObjectStore object_store(repository->git_dir());

  auto fetch_result = FetchRepositoryData(repo_url);
  if (!fetch_result) {
    return UnexpectedError(fetch_result.error());
  }

  GitPackProcessor pack_processor;
  auto head_commit_body = pack_processor.UnpackAndStore(
      fetch_result->first, fetch_result->second, object_store);
  if (!head_commit_body) {
    return UnexpectedError(head_commit_body.error());
  }

  auto tree_hash = ExtractCommitTreeHash(*head_commit_body);
  if (!tree_hash) {
    return UnexpectedError(tree_hash.error());
  }

  return RestoreTreeRecursive(*tree_hash, destination, object_store);
}

}  // namespace gitcpp
