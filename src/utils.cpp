#include "git-cpp/utils.hpp"

#define CHUNK 16384  // 16KB

std::string zlib_decompress_string(const std::string& compressed_data) {
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

void zlib_compress(const std::string& data, uLong* bound, Bytef* dest) {
  int ret = compress(dest, bound, reinterpret_cast<const Bytef*>(data.c_str()), data.size());
  if (ret != Z_OK) {
    throw std::runtime_error("Compression failed: " + std::to_string(ret));
  }
}

std::string zlib_compress_string(const std::string& input_str) {
  z_stream c_stream;
  memset(&c_stream, 0, sizeof(c_stream));

  if (deflateInit(&c_stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
    throw(std::runtime_error("deflateInit failed while compressing."));
  }

  c_stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input_str.data()));
  c_stream.avail_in = input_str.size();

  int status;
  const size_t buffer_size = 32768;  // 32KB
  char buffer[buffer_size];
  std::string compressed_str;

  do {
    c_stream.next_out = reinterpret_cast<Bytef*>(buffer);
    c_stream.avail_out = sizeof(buffer);

    status = deflate(&c_stream, Z_FINISH);

    if (compressed_str.size() < c_stream.total_out) {
      compressed_str.append(buffer, c_stream.total_out - compressed_str.size());
    }
  } while (status == Z_OK);

  if (deflateEnd(&c_stream) != Z_OK) {
    throw(std::runtime_error("deflateEnd failed while compressing."));
  }

  if (status != Z_STREAM_END) {
    std::ostringstream oss;
    oss << "Exception during zlib compression: (" << status << ") " << c_stream.msg;
    throw(std::runtime_error(oss.str()));
  }

  return compressed_str;
}

bool zlib_compress_file(FILE* input, FILE* output) {
  // Initialize compression stream.
  // std::cout << "Initializing compression stream.\n";
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
    std::cerr << "Failed to initialize compression stream.\n";
    return false;
  }

  char in[CHUNK];
  char out[CHUNK];
  int ret;
  int flush;

  do {
    stream.avail_in = fread(in, 1, CHUNK, input);
    stream.next_in = reinterpret_cast<unsigned char*>(in);
    if (ferror(input)) {
      (void)deflateEnd(&stream);  // Free memory
      std::cerr << "Failed to read from input file.\n";
      return false;
    }
    flush = feof(input) ? Z_FINISH : Z_NO_FLUSH;

    do {
      stream.avail_out = CHUNK;
      stream.next_out = reinterpret_cast<unsigned char*>(out);
      ret = deflate(&stream, flush);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        (void)deflateEnd(&stream);  // Free memory
        std::cerr << "Failed to compress file.\n";
        return false;
      }
      size_t have = CHUNK - stream.avail_out;
      if (fwrite(out, 1, have, output) != have || ferror(output)) {
        (void)deflateEnd(&stream);  // Free memory
        std::cerr << "Failed to write to output file.\n";
        return false;
      }
    } while (stream.avail_out == 0);
  } while (flush != Z_FINISH);

  // Clean up and check for errors
  if (deflateEnd(&stream) != Z_OK) {
    return false;
  }

  return true;
}

bool zlib_decompress_file(FILE* input, FILE* output) {
  // initialize decompression stream
  // std::cout << "Initializing decompression stream.\n";
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  if (inflateInit(&stream) != Z_OK) {
    std::cerr << "Failed to initialize decompression stream.\n";
    return false;
  }

  // initialize decompression variables
  char in[CHUNK];
  char out[CHUNK];
  bool haveHeader = false;
  char header[64];
  int ret;

  do {
    stream.avail_in = fread(in, 1, CHUNK, input);           // read from input file
    stream.next_in = reinterpret_cast<unsigned char*>(in);  // set input stream
    if (ferror(input)) {
      std::cerr << "Failed to read from input file.\n";
      return false;
    }
    if (stream.avail_in == 0) {
      break;
    }

    do {
      stream.avail_out = CHUNK;                                 // set output buffer size
      stream.next_out = reinterpret_cast<unsigned char*>(out);  // set output stream
      ret = inflate(&stream, Z_NO_FLUSH);                       // decompress
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        std::cerr << "Failed to decompress file.\n";
        return false;
      }

      // write header to output file
      unsigned headerLen = 0, dataLen = 0;
      if (!haveHeader) {
        sscanf(out, "%s %u", header, &dataLen);
        haveHeader = true;
        headerLen = strlen(out) + 1;
      }
      // write decompressed data to output file
      if (dataLen > 0) {
        if (fwrite(out + headerLen, 1, dataLen, output) != dataLen) {
          std::cerr << "Failed to write to output file.\n";
          return false;
        }
      }
    } while (stream.avail_out == 0);

  } while (ret != Z_STREAM_END);

  return inflateEnd(&stream) == Z_OK;
}

std::string compute_sha1(const std::string& data) {
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

std::string hash_to_hex(const std::string& hash) {
  std::stringstream ss;
  for (unsigned char c : hash) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(static_cast<unsigned char>(c));
  }
  return ss.str();
}
