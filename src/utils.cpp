#include "git-cpp/utils.hpp"

std::string zlib_decompress(const std::string& compressed_data) {
  z_stream zs = {};

  if (inflateInit(&zs) != Z_OK) {
    throw std::runtime_error("inflateInit failed");
  }

  // zlib expects a non-const pointer
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed_data.data()));
  zs.avail_in = static_cast<uInt>(compressed_data.size());

  int ret;
  std::string decompressed;

  do {
    constexpr size_t buffer_size = 32768;
    char out_buffer[buffer_size];
    zs.next_out = reinterpret_cast<Bytef*>(out_buffer);
    zs.avail_out = buffer_size;

    ret = inflate(&zs, 0);
    if (decompressed.size() < zs.total_out) {
      decompressed.append(out_buffer, zs.total_out - decompressed.size());
    }
  } while (ret == Z_OK);

  inflateEnd(&zs);

  if (ret != Z_STREAM_END) {
    throw std::runtime_error("inflate failed: " + std::to_string(ret));
  }

  return decompressed;
}

void zlib_compress_file(const std::string& data, uLong* bound, Bytef* dest) {
  int ret = compress(dest, bound, reinterpret_cast<const Bytef*>(data.c_str()), data.size());
  if (ret != Z_OK) {
    throw std::runtime_error("Compression failed: " + std::to_string(ret));
  }
}


std::string sha_file(const std::string& data) {
  unsigned char hash[SHA_DIGEST_LENGTH];
  // Compute SHA1 hash
  SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);

  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (const unsigned char i : hash) {
    ss << std::setw(2) << static_cast<int>(i);
  }
  return ss.str();
}
