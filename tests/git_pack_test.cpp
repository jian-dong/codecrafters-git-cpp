#include "git-cpp/git_object_store.hpp"
#include "git-cpp/git_pack.hpp"
#include "git-cpp/utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string PackHeader(std::uint32_t object_count) {
  std::string out = "PACK";
  out.push_back(static_cast<char>(0x00));
  out.push_back(static_cast<char>(0x00));
  out.push_back(static_cast<char>(0x00));
  out.push_back(static_cast<char>(0x02));
  out.push_back(static_cast<char>((object_count >> 24) & 0xFF));
  out.push_back(static_cast<char>((object_count >> 16) & 0xFF));
  out.push_back(static_cast<char>((object_count >> 8) & 0xFF));
  out.push_back(static_cast<char>(object_count & 0xFF));
  return out;
}

std::string EncodeObjectHeader(int type, std::size_t size) {
  std::string out;
  unsigned char first = static_cast<unsigned char>((type & 0x7) << 4);
  first |= static_cast<unsigned char>(size & 0x0F);
  size >>= 4;

  if (size != 0) {
    first |= 0x80U;
  }
  out.push_back(static_cast<char>(first));

  while (size != 0) {
    unsigned char next = static_cast<unsigned char>(size & 0x7FU);
    size >>= 7;
    if (size != 0) {
      next |= 0x80U;
    }
    out.push_back(static_cast<char>(next));
  }

  return out;
}

struct ScopedTempDir {
  fs::path path;

  explicit ScopedTempDir(fs::path value) : path(std::move(value)) {}

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

ScopedTempDir MakeTempDir() {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path dir = fs::temp_directory_path() / ("gitcpp-pack-test-" + std::to_string(tick));
  std::error_code ec;
  fs::create_directories(dir, ec);
  return ScopedTempDir(dir);
}

TEST(GitPackProcessorTest, ReturnsErrorWhenPackSignatureIsMissing) {
  const auto tmp = MakeTempDir();
  gitcpp::GitObjectStore store(tmp.path / ".git");
  gitcpp::GitPackProcessor processor;

  auto result = processor.UnpackAndStore("no-pack-data", std::string(40, '0'), store);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, gitcpp::GitErrorCode::kPackSignatureMissing);
}

TEST(GitPackProcessorTest, ReturnsErrorForUnsupportedOffsetDelta) {
  const auto tmp = MakeTempDir();
  gitcpp::GitObjectStore store(tmp.path / ".git");
  gitcpp::GitPackProcessor processor;

  std::string response = PackHeader(1);
  response += EncodeObjectHeader(6, 0);

  auto result = processor.UnpackAndStore(response, std::string(40, '0'), store);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, gitcpp::GitErrorCode::kPackDeltaUnsupported);
}

TEST(GitPackProcessorTest, StoresObjectAndReturnsHeadBody) {
  const auto tmp = MakeTempDir();
  gitcpp::GitObjectStore store(tmp.path / ".git");
  gitcpp::GitPackProcessor processor;

  const std::string blob_body = "hello";
  const std::string raw_blob("blob 5\0hello", 12);
  const std::string blob_hash = gitcpp::ComputeSha1(raw_blob);

  auto compressed_payload = gitcpp::ZlibCompressString(blob_body);
  ASSERT_TRUE(compressed_payload.has_value());

  std::string response = PackHeader(1);
  response += EncodeObjectHeader(3, blob_body.size());
  response += *compressed_payload;

  auto result = processor.UnpackAndStore(response, blob_hash, store);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, blob_body);

  auto exists = store.ObjectExists(blob_hash);
  ASSERT_TRUE(exists.has_value());
  EXPECT_TRUE(*exists);
}

}  // namespace
