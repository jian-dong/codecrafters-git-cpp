#include "git-cpp/git_object_format.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(HexToBytesTest, DecodesValidHex) {
  auto bytes = gitcpp::HexToBytes("4869");
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, "Hi");
}

TEST(HexToBytesTest, RejectsOddLengthInput) {
  auto odd_length = gitcpp::HexToBytes("abc");
  ASSERT_FALSE(odd_length.has_value());
  EXPECT_EQ(odd_length.error().code, gitcpp::GitErrorCode::kInvalidArgument);
}

TEST(ParseTreeEntriesTest, ParsesTreeBody) {
  std::string tree;
  tree += "100644 hello.txt";
  tree.push_back('\0');
  tree += std::string(20, static_cast<char>(0x01));
  tree += "40000 src";
  tree.push_back('\0');
  tree += std::string(20, static_cast<char>(0xFF));

  auto entries = gitcpp::ParseTreeEntries(tree);
  ASSERT_TRUE(entries.has_value());
  ASSERT_EQ(entries->size(), 2U);

  EXPECT_EQ((*entries)[0].mode, "100644");
  EXPECT_EQ((*entries)[0].name, "hello.txt");
  EXPECT_EQ((*entries)[0].hash_hex, "0101010101010101010101010101010101010101");

  EXPECT_EQ((*entries)[1].mode, "40000");
  EXPECT_EQ((*entries)[1].name, "src");
  EXPECT_EQ((*entries)[1].hash_hex, std::string(40, 'f'));
}

TEST(ParseTreeEntriesTest, RejectsMalformedTreeBody) {
  auto entries = gitcpp::ParseTreeEntries("100644 missing-null");
  ASSERT_FALSE(entries.has_value());
  EXPECT_EQ(entries.error().code, gitcpp::GitErrorCode::kTreeFormatInvalid);
}

TEST(ExtractCommitTreeHashTest, ParsesValidCommit) {
  const std::string tree_hash = "0123456789abcdef0123456789abcdef01234567";
  const std::string commit_body =
      "tree " + tree_hash +
      "\nauthor Jane <jane@example.com> 0 +0000\n"
      "committer Jane <jane@example.com> 0 +0000\n\nmsg\n";

  auto extracted = gitcpp::ExtractCommitTreeHash(commit_body);
  ASSERT_TRUE(extracted.has_value());
  EXPECT_EQ(*extracted, tree_hash);
}

TEST(ExtractCommitTreeHashTest, RejectsInvalidTreeHash) {
  auto invalid = gitcpp::ExtractCommitTreeHash("tree zzz\n");
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().code, gitcpp::GitErrorCode::kCommitFormatInvalid);
}

}  // namespace
