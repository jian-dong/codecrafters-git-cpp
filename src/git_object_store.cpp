#include "git-cpp/git_object_store.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

#include "git-cpp/git_fs.hpp"
#include "git-cpp/utils.hpp"

namespace gitcpp {
namespace {

constexpr std::size_t kHashHexLength = 40;

}  // namespace

GitObjectStore::GitObjectStore(std::filesystem::path git_dir)
    : git_dir_(std::move(git_dir)) {}

const std::filesystem::path& GitObjectStore::git_dir() const noexcept {
  return git_dir_;
}

GitExpected<bool> GitObjectStore::ObjectExists(std::string_view object_hash) const {
  auto path = ObjectPath(object_hash);
  if (!path) {
    return UnexpectedError(path.error());
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(*path, ec);
  if (ec) {
    return UnexpectedSystemError(GitErrorCode::kIoError,
                                 "object exists: query object path",
                                 path->string(), ec,
                                 "Failed to check object file");
  }
  return exists;
}

GitExpected<std::string> GitObjectStore::ReadRawObject(
    std::string_view object_hash) const {
  auto path = ObjectPath(object_hash);
  if (!path) {
    return UnexpectedError(path.error());
  }

  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    if (ec) {
      return UnexpectedSystemError(GitErrorCode::kIoError,
                                   "read raw object: query object path",
                                   path->string(), ec,
                                   "Failed to access object file");
    }
    return UnexpectedError(GitErrorCode::kObjectNotFound, "Object not found",
                           "read raw object", std::string(object_hash));
  }

  auto compressed_data = ReadBinaryFile(*path);
  if (!compressed_data) {
    return UnexpectedError(compressed_data.error());
  }

  auto raw_object = ZlibDecompressString(*compressed_data);
  if (!raw_object) {
    return UnexpectedError(raw_object.error());
  }
  return *raw_object;
}

GitExpected<std::string> GitObjectStore::ReadObjectBody(
    std::string_view object_hash) const {
  auto raw_object = ReadRawObject(object_hash);
  if (!raw_object) {
    return UnexpectedError(raw_object.error());
  }
  return ExtractBody(*raw_object);
}

GitExpected<std::string> GitObjectStore::StoreObject(
    std::string_view object_type, std::string_view object_body) const {
  const std::string raw_object = BuildRawObject(object_type, object_body);
  const std::string object_hash = ComputeSha1(raw_object);
  auto status = StoreRawObject(object_hash, raw_object);
  if (!status) {
    return UnexpectedError(status.error());
  }
  return object_hash;
}

GitStatus GitObjectStore::StoreRawObject(std::string_view object_hash,
                                         std::string_view raw_object) const {
  auto path = ObjectPath(object_hash);
  if (!path) {
    return UnexpectedError(path.error());
  }

  std::error_code exists_ec;
  if (std::filesystem::exists(*path, exists_ec)) {
    return {};
  }
  if (exists_ec) {
    return UnexpectedSystemError(GitErrorCode::kIoError,
                                 "store raw object: query object path",
                                 path->string(), exists_ec,
                                 "Failed to check object file");
  }

  auto compressed_data = ZlibCompressString(std::string(raw_object));
  if (!compressed_data) {
    return UnexpectedError(compressed_data.error());
  }

  return WriteBinaryFile(*path, *compressed_data);
}

GitExpected<std::filesystem::path> GitObjectStore::ObjectPath(
    std::string_view object_hash) const {
  auto status = ValidateObjectHash(object_hash);
  if (!status) {
    return UnexpectedError(status.error());
  }
  return git_dir_ / "objects" / std::string(object_hash.substr(0, 2)) /
         std::string(object_hash.substr(2));
}

GitStatus GitObjectStore::ValidateObjectHash(std::string_view object_hash) {
  if (object_hash.size() != kHashHexLength) {
    return UnexpectedError(GitErrorCode::kObjectHashInvalid,
                           "Invalid object hash length",
                           "validate object hash", std::string(object_hash));
  }

  for (char ch : object_hash) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
      return UnexpectedError(GitErrorCode::kObjectHashInvalid, "Invalid object hash",
                             "validate object hash", std::string(object_hash));
    }
  }
  return {};
}

std::string GitObjectStore::BuildRawObject(std::string_view object_type,
                                           std::string_view object_body) {
  return std::string(object_type) + " " + std::to_string(object_body.size()) + '\0' +
         std::string(object_body);
}

GitExpected<std::string> GitObjectStore::ExtractBody(std::string_view raw_object) {
  const std::size_t null_pos = raw_object.find('\0');
  if (null_pos == std::string_view::npos) {
    return UnexpectedError(GitErrorCode::kObjectFormatInvalid, "Invalid git object format",
                           "extract object body");
  }
  return std::string(raw_object.substr(null_pos + 1));
}

}  // namespace gitcpp
