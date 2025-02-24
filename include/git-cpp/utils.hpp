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

inline std::string hash_to_hex(const std::string& hash) {
  std::stringstream ss;
  for (unsigned char c : hash) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(static_cast<unsigned char>(c));
  }
  return ss.str();
}

inline std::string get_sha1_raw_for_string(const std::string& data) {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  EVP_DigestInit_ex(context, EVP_sha1(), NULL);
  EVP_DigestUpdate(context, data.c_str(), data.length());
  unsigned char hash[20];
  unsigned int length;
  EVP_DigestFinal_ex(context, hash, &length);
  EVP_MD_CTX_free(context);
  return std::string(reinterpret_cast<char*>(hash), 20);
}
