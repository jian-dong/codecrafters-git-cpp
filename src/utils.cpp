#include "git-cpp/utils.hpp"

#include <openssl/sha.h>

#include <array>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

namespace gitcpp {
namespace {

constexpr std::size_t kChunkSize = 16 * 1024;

GitExpected<std::string> ReadAll(FILE* file) {
  if (file == nullptr) {
    return UnexpectedError(GitErrorCode::kInvalidArgument, "Invalid file handle",
                           "read all");
  }

  std::array<char, kChunkSize> buffer{};
  std::string data;
  while (true) {
    const std::size_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);
    if (bytes_read > 0) {
      data.append(buffer.data(), bytes_read);
    }
    if (bytes_read < buffer.size()) {
      if (ferror(file) != 0) {
        return UnexpectedError(GitErrorCode::kIoError, "Failed to read from file",
                               "read all");
      }
      break;
    }
  }
  return data;
}

}  // namespace

GitExpected<std::string> ZlibDecompressString(const std::string& compressed_data) {
  if (compressed_data.empty()) {
    return std::string{};
  }

  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) {
    return UnexpectedError(GitErrorCode::kCompressionError, "inflateInit failed",
                           "zlib decompress");
  }

  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed_data.data()));
  stream.avail_in = static_cast<uInt>(compressed_data.size());

  std::array<char, kChunkSize> output_buffer{};
  std::string decompressed_data;
  int status = Z_OK;

  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(output_buffer.data());
    stream.avail_out = static_cast<uInt>(output_buffer.size());
    status = inflate(&stream, Z_NO_FLUSH);

    if (status != Z_OK && status != Z_STREAM_END) {
      (void)inflateEnd(&stream);
      return UnexpectedError(GitErrorCode::kCompressionError,
                             "inflate failed: " + std::to_string(status),
                             "zlib decompress");
    }

    const std::size_t produced_size = output_buffer.size() - stream.avail_out;
    decompressed_data.append(output_buffer.data(), produced_size);
  }

  if (inflateEnd(&stream) != Z_OK) {
    return UnexpectedError(GitErrorCode::kCompressionError, "inflateEnd failed",
                           "zlib decompress");
  }
  return decompressed_data;
}

GitExpected<std::string> ZlibCompressString(const std::string& input_string) {
  z_stream stream{};
  if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
    return UnexpectedError(GitErrorCode::kCompressionError, "deflateInit failed",
                           "zlib compress");
  }

  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(input_string.data()));
  stream.avail_in = static_cast<uInt>(input_string.size());

  std::array<char, kChunkSize> output_buffer{};
  std::string compressed_data;
  int status = Z_OK;

  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(output_buffer.data());
    stream.avail_out = static_cast<uInt>(output_buffer.size());
    status = deflate(&stream, Z_FINISH);

    if (status != Z_OK && status != Z_STREAM_END) {
      (void)deflateEnd(&stream);
      return UnexpectedError(GitErrorCode::kCompressionError,
                             "deflate failed: " + std::to_string(status),
                             "zlib compress");
    }

    const std::size_t produced_size = output_buffer.size() - stream.avail_out;
    compressed_data.append(output_buffer.data(), produced_size);
  }

  if (deflateEnd(&stream) != Z_OK) {
    return UnexpectedError(GitErrorCode::kCompressionError, "deflateEnd failed",
                           "zlib compress");
  }
  return compressed_data;
}

GitStatus ZlibCompressFile(FILE* input, FILE* output) {
  auto raw_data = ReadAll(input);
  if (!raw_data) {
    return UnexpectedError(raw_data.error());
  }

  auto compressed_data = ZlibCompressString(*raw_data);
  if (!compressed_data) {
    return UnexpectedError(compressed_data.error());
  }

  const std::size_t bytes_written =
      fwrite(compressed_data->data(), 1, compressed_data->size(), output);
  if (bytes_written != compressed_data->size()) {
    return UnexpectedError(GitErrorCode::kIoError,
                           "Failed to write compressed data",
                           "zlib compress file");
  }
  return {};
}

GitStatus ZlibDecompressFile(FILE* input, FILE* output) {
  auto compressed_data = ReadAll(input);
  if (!compressed_data) {
    return UnexpectedError(compressed_data.error());
  }

  auto decompressed_data = ZlibDecompressString(*compressed_data);
  if (!decompressed_data) {
    return UnexpectedError(decompressed_data.error());
  }

  const std::size_t null_pos = decompressed_data->find('\0');
  if (null_pos == std::string::npos) {
    return UnexpectedError(GitErrorCode::kObjectFormatInvalid, "Invalid git object format",
                           "zlib decompress file");
  }

  const std::string_view payload(decompressed_data->data() + null_pos + 1,
                                 decompressed_data->size() - null_pos - 1);
  const std::size_t bytes_written = fwrite(payload.data(), 1, payload.size(), output);
  if (bytes_written != payload.size()) {
    return UnexpectedError(GitErrorCode::kIoError,
                           "Failed to write decompressed data",
                           "zlib decompress file");
  }
  return {};
}

std::string ComputeSha1(std::string_view data) {
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const unsigned char byte : hash) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

std::string HashToHex(std::string_view hash_bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const unsigned char byte : hash_bytes) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

}  // namespace gitcpp
