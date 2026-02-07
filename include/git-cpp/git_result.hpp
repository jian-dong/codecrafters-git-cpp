#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "tl/expected.hpp"

namespace gitcpp {

enum class GitErrorCode {
  kUnknown = 0,
  kInvalidArgument,
  kAlreadyExists,
  kIoError,
  kCompressionError,
  kNetworkError,
  kInternalError,
  kRepositoryNotFound,
  kObjectNotFound,
  kTreeNotFound,
  kCommitNotFound,
  kObjectHashInvalid,
  kObjectFormatInvalid,
  kTreeFormatInvalid,
  kCommitFormatInvalid,
  kRemoteRefNotFound,
  kPackSignatureMissing,
  kPackHeaderInvalid,
  kPackStreamTruncated,
  kPackObjectLengthInvalid,
  kPackDeltaInvalid,
  kPackDeltaUnsupported,
  kPackHeadNotFound,
};

struct GitError {
  GitErrorCode code = GitErrorCode::kUnknown;
  std::string message;
  std::string operation;
  std::string resource;
  std::optional<int> native_code;
  std::string native_message;
  std::vector<std::string> context;
};

template <typename T>
using GitExpected = tl::expected<T, GitError>;

using GitStatus = tl::expected<void, GitError>;

inline std::string_view GitErrorCodeName(GitErrorCode code) {
  switch (code) {
    case GitErrorCode::kUnknown:
      return "unknown";
    case GitErrorCode::kInvalidArgument:
      return "invalid_argument";
    case GitErrorCode::kAlreadyExists:
      return "already_exists";
    case GitErrorCode::kIoError:
      return "io_error";
    case GitErrorCode::kCompressionError:
      return "compression_error";
    case GitErrorCode::kNetworkError:
      return "network_error";
    case GitErrorCode::kInternalError:
      return "internal_error";
    case GitErrorCode::kRepositoryNotFound:
      return "repository_not_found";
    case GitErrorCode::kObjectNotFound:
      return "object_not_found";
    case GitErrorCode::kTreeNotFound:
      return "tree_not_found";
    case GitErrorCode::kCommitNotFound:
      return "commit_not_found";
    case GitErrorCode::kObjectHashInvalid:
      return "object_hash_invalid";
    case GitErrorCode::kObjectFormatInvalid:
      return "object_format_invalid";
    case GitErrorCode::kTreeFormatInvalid:
      return "tree_format_invalid";
    case GitErrorCode::kCommitFormatInvalid:
      return "commit_format_invalid";
    case GitErrorCode::kRemoteRefNotFound:
      return "remote_ref_not_found";
    case GitErrorCode::kPackSignatureMissing:
      return "pack_signature_missing";
    case GitErrorCode::kPackHeaderInvalid:
      return "pack_header_invalid";
    case GitErrorCode::kPackStreamTruncated:
      return "pack_stream_truncated";
    case GitErrorCode::kPackObjectLengthInvalid:
      return "pack_object_length_invalid";
    case GitErrorCode::kPackDeltaInvalid:
      return "pack_delta_invalid";
    case GitErrorCode::kPackDeltaUnsupported:
      return "pack_delta_unsupported";
    case GitErrorCode::kPackHeadNotFound:
      return "pack_head_not_found";
  }
  return "unknown";
}

inline GitError MakeError(GitErrorCode code, std::string message,
                          std::string operation = {},
                          std::string resource = {}) {
  GitError error;
  error.code = code;
  error.message = std::move(message);
  error.operation = std::move(operation);
  error.resource = std::move(resource);
  return error;
}

inline GitError MakeSystemError(GitErrorCode code, std::string operation,
                                std::string resource,
                                const std::error_code& system_error,
                                std::string message = {}) {
  GitError error = MakeError(code, std::move(message), std::move(operation),
                             std::move(resource));
  error.native_code = system_error.value();
  error.native_message = system_error.message();
  if (error.message.empty()) {
    error.message = error.native_message;
  }
  return error;
}

inline GitError WithContext(GitError error, std::string context) {
  if (!context.empty()) {
    error.context.push_back(std::move(context));
  }
  return error;
}

inline tl::unexpected<GitError> UnexpectedError(GitError error) {
  return tl::make_unexpected(std::move(error));
}

inline tl::unexpected<GitError> UnexpectedError(GitErrorCode code, std::string message) {
  return tl::make_unexpected(MakeError(code, std::move(message)));
}

inline tl::unexpected<GitError> UnexpectedError(GitErrorCode code,
                                                std::string message,
                                                std::string operation) {
  return tl::make_unexpected(
      MakeError(code, std::move(message), std::move(operation)));
}

inline tl::unexpected<GitError> UnexpectedError(GitErrorCode code,
                                                std::string message,
                                                std::string operation,
                                                std::string resource) {
  return tl::make_unexpected(
      MakeError(code, std::move(message), std::move(operation), std::move(resource)));
}

inline tl::unexpected<GitError> UnexpectedSystemError(
    GitErrorCode code, std::string operation, std::string resource,
    const std::error_code& system_error, std::string message = {}) {
  return tl::make_unexpected(
      MakeSystemError(code, std::move(operation), std::move(resource), system_error,
                      std::move(message)));
}

}  // namespace gitcpp
