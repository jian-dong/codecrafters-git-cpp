#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <vector>
#include <zlib.h>
#include <openssl/sha.h>

/**
 * @brief Decompresses zlib-compressed data.
 *
 * @param compressed_data The input compressed string.
 * @return The decompressed string.
 * @throws std::runtime_error if decompression fails.
 */
std::string zlib_decompress(const std::string& compressed_data);

/**
 * @brief Compresses the given data using zlib.
 *
 * @param data The input data to compress.
 * @param bound Pointer to the size of the destination buffer (will be updated with the actual compressed size).
 * @param dest The destination buffer to hold compressed data.
 * @throws std::runtime_error if compression fails.
 */
void zlib_compress_file(const std::string& data, uLong* bound, unsigned char* dest);


/**
 * @brief Computes the SHA1 hash of the given data and returns it as a hexadecimal string.
 *
 * @param data The input data.
 * @return The SHA1 hash in hex.
 */
std::string sha_file(const std::string& data);
