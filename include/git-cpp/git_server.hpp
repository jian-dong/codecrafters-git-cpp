#pragma once

#include "utils.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;


/**
 * @brief Recursively searches for a .git directory in the current or parent directories.
 *
 * @param path The starting path (default is the current path).
 * @return The path to the .git directory, or an empty path if not found.
 */
fs::path find_git_root(fs::path path = fs::current_path());

/**
 * @brief Reads a Git object file and returns its content (skipping the header).
 *
 * @param git_dir The .git directory.
 * @param object_hash The hash of the object.
 * @return The object content.
 * @throws std::runtime_error if the object is not found or is invalid.
 */
std::string read_object_content(const fs::path& git_dir, const std::string& object_hash);


/**
 * @brief Reads a file, constructs a Git blob object, compresses it, writes it to the repository,
 * and returns the computed object hash.
 *
 * @param file_path The path to the file.
 * @return The SHA1 hash of the blob object.
 */
std::string hash_object(const std::string& file_path);

/**
 * @brief Reads a tree object, parses its entries, and returns the names (sorted) each on一行.
 *
 * @param git_dir The .git directory.
 * @param tree_hash The hash of the tree object.
 * @return A string with each entry name followed by a newline.
 */
std::string read_tree_object(const fs::path& git_dir, const std::string& tree_hash);

/**
 * @brief Helper: Converts a hex string to a binary string.
 *
 * @param hex The hex string (expected even length).
 * @return A string containing the binary data.
 */
std::string hex_to_bytes(const std::string& hex);


/**
 * @brief Recursively writes the current directory structure as a Git tree object.
 *
 * For each entry in the directory (skipping .git):
 *   - For regular files, calls hash_object to create a blob (mode "100644").
 *   - For directories, recursively calls write_tree (mode "40000").
 *
 * The tree object is constructed as:
 *   "tree <size>\0" + (for each entry: "<mode> <filename>\0" + raw 20-byte hash)
 *
 * @param directory The directory path to write as a tree.
 * @return The SHA1 hash of the tree object.
 * @throws std::runtime_error if writing fails.
 */
std::string write_tree(const fs::path& directory);

void handle_git_init(const fs::path& repo_path);

void handle_git_cat_file(const fs::path& git_dir, const std::string& object_hash);

void handle_git_hash_object(const fs::path& file_path);

void handle_git_write_tree(const fs::path& dir = fs::current_path());

void handle_git_ls_tree(const fs::path& git_dir, const std::string& tree_hash);

void handle_git_commit_tree(const fs::path& git_dir, const std::string& tree_hash, const std::string& parent_hash,
                            const std::string& commit_message);
