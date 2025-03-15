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
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <openssl/evp.h>
#include <sstream>
#include <cstring>
#include <ctime>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <map>

/**
 * @brief Decompresses zlib-compressed data.
 *
 * @param compressed_data The input compressed string.
 * @return The decompressed string.
 * @throws std::runtime_error if decompression fails.
 */
std::string zlib_decompress_string(const std::string& compressed_data);

/**
 * @brief Compresses the given data using zlib.
 *
 * @param data The input data to compress.
 * @param bound Pointer to the size of the destination buffer (will be updated with the actual
 * compressed size).
 * @param dest The destination buffer to hold compressed data.
 * @throws std::runtime_error if compression fails.
 */
void zlib_compress(const std::string& data, uLong* bound, unsigned char* dest);

std::string zlib_compress_string(const std::string& input_str);

bool zlib_decompress_file(FILE* input, FILE* output);

bool zlib_compress_file(FILE* input, FILE* output);


/**
 * @brief Computes the SHA1 hash of the given data and returns it as a hexadecimal string.
 *
 * @param data The input data.
 * @return The SHA1 hash in hex.
 */
std::string compute_sha1(const std::string& data);

std::string hash_to_hex(const std::string& hash);
