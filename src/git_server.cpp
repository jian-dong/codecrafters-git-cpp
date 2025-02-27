#include "git-cpp/git_server.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <filesystem>
#include <set>
#include <queue>

namespace fs = std::filesystem;

fs::path find_git_root(fs::path path) {
  while (!path.empty()) {
    if (fs::exists(path / ".git") && fs::is_directory(path / ".git")) {
      return path / ".git";
    }
    if (path.has_parent_path()) {
      path = path.parent_path();
    } else {
      return "";
    }
  }
  return "";
}



std::string hash_object(const std::string& file_path) {
  // Read file contents
  std::ifstream file_stream(file_path, std::ios::binary);
  if (!file_stream.is_open()) {
    throw std::runtime_error("Failed to open file: " + file_path);
  }
  std::stringstream buffer;
  buffer << file_stream.rdbuf();
  file_stream.close();
  std::string file_contents = buffer.str();

  // Construct blob object: "blob <size>\0<content>"
  std::string blob_content = "blob " + std::to_string(file_contents.size()) + '\0' + file_contents;

  // Compute SHA1 hash of the blob
  std::string object_hash = sha_file(blob_content);

  // Compress the blob content
  uLong compress_bound_size = compressBound(blob_content.size());
  std::vector<unsigned char> compressed_data(compress_bound_size);
  zlib_compress_file(blob_content, &compress_bound_size, compressed_data.data());

  // Create directory .git/objects/XX where XX are the first two characters of the hash
  std::string dir = ".git/objects/" + object_hash.substr(0, 2);
  fs::create_directories(dir);

  // Construct the object file path
  std::string object_path = dir + "/" + object_hash.substr(2);
  std::ofstream object_file(object_path, std::ios::binary);
  if (!object_file.is_open()) {
    throw std::runtime_error("Failed to open object file for writing: " + object_path);
  }
  object_file.write(reinterpret_cast<const char*>(compressed_data.data()), compress_bound_size);
  object_file.close();

  return object_hash;
}

std::string read_tree_object(const fs::path& git_dir, const std::string& tree_hash) {
  fs::path object_path = git_dir / "objects" / tree_hash.substr(0, 2) / tree_hash.substr(2);
  if (!fs::exists(object_path)) {
    throw std::runtime_error("Object not found: " + tree_hash);
  }
  std::ifstream object_file(object_path, std::ios::binary);
  if (!object_file.is_open()) {
    throw std::runtime_error("Failed to open object file: " + object_path.string());
  }
  std::string compressed_content((std::istreambuf_iterator<char>(object_file)),
                                 std::istreambuf_iterator<char>());
  object_file.close();

  // Decompress the content
  z_stream zStream;
  zStream.zalloc = Z_NULL;
  zStream.zfree = Z_NULL;
  zStream.opaque = Z_NULL;
  zStream.avail_in = compressed_content.size();
  zStream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed_content.data()));

  if (inflateInit2(&zStream, 15 + 32) != Z_OK) {
    // 15 + 32 for gzip and zlib both, auto detect
    throw std::runtime_error("Failed to initialize zlib inflate");
  }

  std::string decompressed_content;
  int ret;
  do {
    char buffer[1024];
    zStream.avail_out = sizeof(buffer);
    zStream.next_out = reinterpret_cast<Bytef*>(buffer);

    ret = inflate(&zStream, Z_SYNC_FLUSH);  // Z_SYNC_FLUSH to handle potential errors

    if (ret < 0 && ret != Z_BUF_ERROR && ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zStream);
      throw std::runtime_error("Zlib inflate error: " + std::to_string(ret));
    }

    if (ret == Z_BUF_ERROR || ret == Z_OK || ret == Z_STREAM_END) {
      decompressed_content.append(buffer, sizeof(buffer) - zStream.avail_out);
    }
  } while (ret != Z_STREAM_END && zStream.avail_out == 0);

  inflateEnd(&zStream);

  // Parse the content
  std::istringstream content_stream(decompressed_content);
  std::string type;
  size_t size;
  char nullChar;
  content_stream >> type >> size >> std::noskipws >> nullChar;  // Read "tree size\0"
  if (type != "tree" || nullChar != '\0') {
    throw std::runtime_error("Object is not a tree or has invalid format");
  }

  std::string entries_data = decompressed_content.substr(content_stream.tellg());
  std::string result;
  std::vector<std::string> names;
  size_t pos = 0;
  while (pos < entries_data.size()) {
    size_t space_pos = entries_data.find(' ', pos);
    if (space_pos == std::string::npos) break;
    size_t nullPos = entries_data.find('\0', space_pos + 1);
    if (nullPos == std::string::npos) break;

    std::string name = entries_data.substr(space_pos + 1, nullPos - (space_pos + 1));
    names.push_back(name);
    pos = nullPos + 21;  // Move position to after null byte and 20-byte hash
  }
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    result += n + "\n";
  }

  return result;
}

std::string hex_to_bytes(const std::string& hex) {
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    std::string byteStr = hex.substr(i, 2);
    char byte = static_cast<char>(std::stoi(byteStr, nullptr, 16));
    bytes.push_back(byte);
  }
  return bytes;
}

std::string write_tree(const fs::path& directory) {
  struct TreeEntry {
    std::string mode;
    std::string name;
    std::string hash;  // 40-character hex string
  };
  std::vector<TreeEntry> entries;

  for (const auto& entry : fs::directory_iterator(directory)) {
    // Skip .git 目录
    if (entry.path().filename() == ".git") continue;

    if (fs::is_directory(entry.path())) {
      std::string tree_hash = write_tree(entry.path());
      entries.push_back({"40000", entry.path().filename().string(), tree_hash});
    } else if (fs::is_regular_file(entry.path())) {
      std::string blobHash = hash_object(entry.path().string());
      entries.push_back({"100644", entry.path().filename().string(), blobHash});
    }
    // 其他类型（如符号链接）可以按需求处理
  }

  // 按文件名排序
  std::sort(entries.begin(), entries.end(),
            [](const TreeEntry& a, const TreeEntry& b) { return a.name < b.name; });

  // 构造 tree body：每个 entry 为 "<mode> <filename>\0" + raw hash(20 字节)
  std::string tree_body;
  for (const auto& e : entries) {
    tree_body += e.mode + " " + e.name + '\0';
    tree_body += hex_to_bytes(e.hash);
  }

  // 构造完整的 tree 对象内容： header + body
  std::string full_content = "tree " + std::to_string(tree_body.size()) + '\0' + tree_body;
  std::string tree_hash = sha_file(full_content);

  // 压缩内容并写入 .git/objects
  uLong compress_bound_size = compressBound(full_content.size());
  std::vector<unsigned char> compressedData(compress_bound_size);
  zlib_compress_file(full_content, &compress_bound_size, compressedData.data());

  fs::path git_dir = find_git_root(fs::current_path());
  if (git_dir.empty()) {
    // 如果没有找到，则默认在当前目录下的 .git 文件夹
    git_dir = fs::current_path() / ".git";
  }
  fs::path object_dir = git_dir / "objects" / tree_hash.substr(0, 2);
  fs::create_directories(object_dir);
  fs::path object_path = object_dir / tree_hash.substr(2);
  std::ofstream object_file(object_path, std::ios::binary);
  if (!object_file.is_open()) {
    throw std::runtime_error("Failed to open object file for writing: " + object_path.string());
  }
  object_file.write(reinterpret_cast<const char*>(compressedData.data()), compress_bound_size);
  object_file.close();

  return tree_hash;
}

void handle_git_init(const fs::path& repo_path) {
  try {
    fs::create_directories(repo_path / ".git/objects");
    fs::create_directories(repo_path / ".git/refs/heads");

    std::ofstream head_file(repo_path / ".git/HEAD");
    if (!head_file.is_open()) {
      std::cerr << "Failed to create .git/HEAD file.\n";
      return;
    }
    head_file << "ref: refs/heads/main\n";
    head_file.close();

    std::cout << "Initialized git directory\n";
  } catch (const fs::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
  }
}

void handle_git_cat_file(const fs::path& git_dir, const std::string& object_hash) {
  try {
    const std::string content = read_object_content(git_dir, object_hash);
    std::cout << content;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }
}

void handle_git_hash_object(const fs::path& file_path) {
  try {
    std::string object_hash = hash_object(file_path);
    std::cout << object_hash << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }
}

void handle_git_write_tree(const fs::path& dir) {
  try {
    std::string tree_hash = write_tree(dir);
    std::cout << tree_hash << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }
}

void handle_git_ls_tree(const fs::path& git_dir, const std::string& tree_hash) {
  try {
    std::string tree_content = read_tree_object(git_dir, tree_hash);
    std::cout << tree_content;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }
}

std::string bytes_to_hex_string(const unsigned char* bytes, size_t length) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < length; i++) {
    ss << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return ss.str();
}

void handle_git_commit_tree(const fs::path& git_dir, const std::string& tree_hash,
                            const std::string& parent_hash, const std::string& commit_message) {
  // Verify if tree object exists
  fs::path tree_path = git_dir / "objects" / tree_hash.substr(0, 2) / tree_hash.substr(2);
  if (!fs::exists(tree_path)) {
    throw std::runtime_error("Tree object not found: " + tree_hash);
  }

  // If parent hash is provided, verify if it exists
  if (!parent_hash.empty()) {
    fs::path parent_path = git_dir / "objects" / parent_hash.substr(0, 2) / parent_hash.substr(2);
    if (!fs::exists(parent_path)) {
      throw std::runtime_error("Parent commit not found: " + parent_hash);
    }
  }

  // Hardcoded user information
  const std::string author = "John Doe <john.doe@gmail.com>";
  const std::string committer = "John Doe <john.doe@gmail.com>";
  const std::string timestamp = std::to_string(std::time(nullptr));

  // Build commit content with correct format
  std::string commit_content = "tree " + tree_hash + "\n";
  if (!parent_hash.empty()) {
    commit_content += "parent " + parent_hash + "\n";
  }
  commit_content += "author " + author + " " + timestamp + " -0800\n" + "committer " + committer +
                    " " + timestamp + " -0800\n" + "\n" + commit_message + "\n";

  // Create header and full content
  std::string header = "commit " + std::to_string(commit_content.length()) + '\0';
  std::string full_content = header + commit_content;

  // Calculate SHA1
  std::string commit_hash = sha_file(full_content);

  // Create object directory
  fs::path object_dir = git_dir / "objects" / commit_hash.substr(0, 2);
  fs::create_directories(object_dir);
  uLong compress_bound_size = compressBound(full_content.size());
  std::vector<unsigned char> compressed_data(compress_bound_size);
  zlib_compress_file(full_content, &compress_bound_size, compressed_data.data());

  // Write compressed content to file
  fs::path object_path = object_dir / commit_hash.substr(2);
  std::ofstream object_file(object_path, std::ios::binary);
  if (!object_file) {
    throw std::runtime_error("Failed to create commit object file");
  }
  object_file.write(reinterpret_cast<const std::ostream::char_type*>(compressed_data.data()),
                    compressed_data.size());
  object_file.close();

  // Output the hash of the new commit
  std::cout << commit_hash << "\n";
}

const std::map<int, std::string> PACK_OBJECT_TYPES = {
    {1, "commit"},   {2, "tree"}, {3, "blob"}, {4, "tag"}, {6, "ofs_delta"},  // offset delta
    {7, "ref_delta"}                                                          // reference delta
};

struct GitRef {
  std::string name;
  std::string hash;
};

std::string get_object_path(const std::string& hash, const std::string& output_path = ".") {
  return output_path + "/.git/objects/" + hash.substr(0, 2) + "/" + hash.substr(2);
}

void store_compressed_data(const std::string& hash, const std::vector<char>& compressed,
                           const std::string& output_path = ".") {
  std::string path = get_object_path(hash, output_path);
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream output_file(path, std::ios::binary);
  if (!output_file) {
    throw std::runtime_error("Cannot open input file");
  }
  output_file.write(compressed.data(), compressed.size());
  output_file.close();
}

std::vector<unsigned char> apply_delta(const std::vector<unsigned char>& base,
                                       const std::vector<unsigned char>& delta) {
  std::vector<unsigned char> result;
  size_t pos = 0;

  // Read source size (variable length)
  size_t source_size = 0;
  size_t shift = 0;
  while (pos < delta.size()) {
    unsigned char byte = delta[pos++];
    source_size |= (byte & 127) << shift;
    if (!(byte & 128)) break;
    shift += 7;
  }

  // Read target size (variable length)
  size_t target_size = 0;
  shift = 0;
  while (pos < delta.size()) {
    unsigned char byte = delta[pos++];
    target_size |= (byte & 127) << shift;
    if (!(byte & 128)) break;
    shift += 7;
  }

  // Apply delta instructions
  while (pos < delta.size()) {
    unsigned char cmd = delta[pos++];
    if (cmd & 128) {  // copy instruction
      size_t offset = 0;
      size_t size = 0;
      if (cmd & 1) offset |= delta[pos++];
      if (cmd & 2) offset |= delta[pos++] << 8;
      if (cmd & 4) offset |= delta[pos++] << 16;
      if (cmd & 8) offset |= delta[pos++] << 24;
      if (cmd & 16) size |= delta[pos++];
      if (cmd & 32) size |= delta[pos++] << 8;
      if (cmd & 64) size |= delta[pos++] << 16;
      if (size == 0) size = 0x10000;

      if (offset + size > base.size()) {
        throw std::runtime_error("Delta copy out of bounds");
      }
      result.insert(result.end(), base.begin() + offset, base.begin() + offset + size);
    } else if (cmd) {  // insert instruction
      if (pos + cmd > delta.size()) {
        throw std::runtime_error("Delta insert out of bounds");
      }
      result.insert(result.end(), delta.begin() + pos, delta.begin() + pos + cmd);
      pos += cmd;
    } else {
      throw std::runtime_error("Invalid delta instruction");
    }
  }

  if (result.size() != target_size) {
    throw std::runtime_error("Delta reconstruction size mismatch");
  }

  return result;
}

// Fixed process_packfile to handle the packfile correctly
void process_packfile(const std::string& pack_data, const std::string& output_path) {
  // Skip the initial "0008NAK\n" response, if present
  size_t pos = 0;
  if (pack_data.starts_with("0008NAK\n")) {
    pos = 8; // Length of "0008NAK\n"
  }

  // Check for PACK signature
  if (pack_data.substr(pos, 4) != "PACK") {
    throw std::runtime_error("Invalid pack signature: " + pack_data.substr(pos, 4));
  }
  pos += 4;

  // Parse version
  uint32_t version;
  memcpy(&version, pack_data.data() + pos, 4);
  version = ntohl(version);
  pos += 4;

  // Parse number of objects
  uint32_t num_objects;
  memcpy(&num_objects, pack_data.data() + pos, 4);
  num_objects = ntohl(num_objects);
  pos += 4;

  std::cout << "Pack version: " << version << ", Objects: " << num_objects << std::endl;

  // Create a map to store unpacked objects for delta resolution
  std::map<std::string, std::vector<unsigned char>> unpacked_objects;

  // Process each object in the packfile
  for (uint32_t i = 0; i < num_objects; i++) {
    // Parse object type and size
    uint8_t byte = static_cast<uint8_t>(pack_data[pos++]);
    int type = (byte >> 4) & 7;
    size_t size = byte & 15;
    int shift = 4;

    // Parse variable-length size encoding
    while (byte & 128) {
      if (pos >= pack_data.size()) {
        throw std::runtime_error("Unexpected end of pack data while parsing object size");
      }
      byte = static_cast<uint8_t>(pack_data[pos++]);
      size |= (static_cast<size_t>(byte & 127) << shift);
      shift += 7;
    }

    // Handle different object types
    if (type >= 1 && type <= 4) {
      // Regular object types: commit, tree, blob, tag

      // Get the start position for the zlib compressed data
     // size_t data_start = pos;

      // Decompress the object data
      z_stream zs;
      memset(&zs, 0, sizeof(zs));
      if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib for object decompression");
      }

      // Set input buffer
      zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(pack_data.data() + pos));
      zs.avail_in = pack_data.size() - pos;

      // Decompress the data
      std::vector<unsigned char> uncompressed;
      unsigned char outbuffer[4096];

      do {
        zs.next_out = outbuffer;
        zs.avail_out = sizeof(outbuffer);

        int ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
          inflateEnd(&zs);
          throw std::runtime_error("Decompression failed: " + std::to_string(ret));
        }

        uncompressed.insert(uncompressed.end(), outbuffer, outbuffer + (sizeof(outbuffer) - zs.avail_out));
      } while (zs.avail_out == 0);

      // Update position
      pos += zs.total_in;
      inflateEnd(&zs);

      // Create the full object content with header
      std::string object_type = PACK_OBJECT_TYPES.at(type);
      std::string full_object = object_type + " " + std::to_string(uncompressed.size()) + '\0';
      full_object.insert(full_object.end(), uncompressed.begin(), uncompressed.end());

      // Calculate the object hash
      std::string hash = sha_file(full_object);

      // Store for potential delta resolution
      unpacked_objects[hash] = std::vector<unsigned char>(full_object.begin(), full_object.end());

      // Compress for storage
      uLong compress_bound_size = compressBound(full_object.size());
      std::vector<char> compressed_data(compress_bound_size);
      z_stream z;
      memset(&z, 0, sizeof(z));
      z.zalloc = Z_NULL;
      z.zfree = Z_NULL;
      z.opaque = Z_NULL;

      if (deflateInit(&z, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib deflate");
      }

      z.avail_in = full_object.size();
      z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(full_object.data()));
      z.avail_out = compress_bound_size;
      z.next_out = reinterpret_cast<Bytef*>(compressed_data.data());

      if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&z);
        throw std::runtime_error("Failed to compress object data");
      }

      compress_bound_size = z.total_out;
      deflateEnd(&z);

      // Resize the compressed data to the actual size
      compressed_data.resize(compress_bound_size);

      // Store the compressed object
      fs::path object_dir = fs::path(output_path) / ".git" / "objects" / hash.substr(0, 2);
      fs::create_directories(object_dir);

      fs::path object_path = object_dir / hash.substr(2);
      std::ofstream object_file(object_path, std::ios::binary);
      if (!object_file) {
        throw std::runtime_error("Failed to create object file: " + object_path.string());
      }

      object_file.write(compressed_data.data(), compressed_data.size());
      object_file.close();

      std::cout << "Stored object " << hash << " (type: " << object_type << ")" << std::endl;
    }
    else if (type == 6) {  // Offset delta
      // Not implemented in this simplified version
      throw std::runtime_error("Offset delta objects not supported yet");
    }
    else if (type == 7) {  // Reference delta
      // Reference delta - base object is identified by its hash
      if (pos + 20 > pack_data.size()) {
        throw std::runtime_error("Pack data too short for ref delta");
      }

      // Extract the base object hash
      std::string base_hash_raw = pack_data.substr(pos, 20);
      pos += 20;

      // Convert raw hash to hex string
      std::string base_hash;
      for (char c : base_hash_raw) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(c));
        base_hash += hex;
      }

      // Decompress the delta data
      z_stream zs;
      memset(&zs, 0, sizeof(zs));
      if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib for delta decompression");
      }

      zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(pack_data.data() + pos));
      zs.avail_in = pack_data.size() - pos;

      std::vector<unsigned char> delta_data;
      unsigned char outbuffer[4096];

      do {
        zs.next_out = outbuffer;
        zs.avail_out = sizeof(outbuffer);

        int ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
          inflateEnd(&zs);
          throw std::runtime_error("Delta decompression failed: " + std::to_string(ret));
        }

        delta_data.insert(delta_data.end(), outbuffer, outbuffer + (sizeof(outbuffer) - zs.avail_out));
      } while (zs.avail_out == 0);

      // Update position
      pos += zs.total_in;
      inflateEnd(&zs);

      // Get the base object content
      if (unpacked_objects.find(base_hash) == unpacked_objects.end()) {
        throw std::runtime_error("Base object not found for delta: " + base_hash);
      }

      std::vector<unsigned char> base_content = unpacked_objects[base_hash];

      // Apply the delta to get the target object
      std::vector<unsigned char> target_content = apply_delta(base_content, delta_data);

      // Get the object type from the base object (header before null byte)
      std::string base_header(base_content.begin(),
                             std::find(base_content.begin(), base_content.end(), '\0'));

      // Extract object type
      std::string object_type = base_header.substr(0, base_header.find(' '));

      // Create full object with header
      std::string full_object = object_type + " " + std::to_string(target_content.size()) + '\0';
      full_object.insert(full_object.end(), target_content.begin(), target_content.end());

      // Calculate hash
      std::string hash = sha_file(full_object);

      // Compress for storage
      uLong compress_bound_size = compressBound(full_object.size());
      std::vector<char> compressed_data(compress_bound_size);
      z_stream z;
      memset(&z, 0, sizeof(z));
      z.zalloc = Z_NULL;
      z.zfree = Z_NULL;
      z.opaque = Z_NULL;

      if (deflateInit(&z, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib deflate for delta result");
      }

      z.avail_in = full_object.size();
      z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(full_object.data()));
      z.avail_out = compress_bound_size;
      z.next_out = reinterpret_cast<Bytef*>(compressed_data.data());

      if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&z);
        throw std::runtime_error("Failed to compress delta result");
      }

      compress_bound_size = z.total_out;
      deflateEnd(&z);

      // Store the compressed object
      fs::path object_dir = fs::path(output_path) / ".git" / "objects" / hash.substr(0, 2);
      fs::create_directories(object_dir);

      fs::path object_path = object_dir / hash.substr(2);
      std::ofstream object_file(object_path, std::ios::binary);
      if (!object_file) {
        throw std::runtime_error("Failed to create object file from delta: " + object_path.string());
      }

      object_file.write(compressed_data.data(), compress_bound_size);
      object_file.close();

      std::cout << "Stored delta-derived object " << hash << " (type: " << object_type << ")" << std::endl;
    }
    else {
      throw std::runtime_error("Unknown object type: " + std::to_string(type));
    }
  }

  // Skip over the pack checksum (20 bytes) at the end
  // We don't validate it in this simplified implementation

  std::cout << "Packfile processing complete." << std::endl;
}

void handle_git_clone(const std::string& repo_url, const fs::path& dest_dir) {
  // Create the destination directory if it doesn't exist
  fs::create_directories(dest_dir);

  // Initialize git repository in the destination directory
  handle_git_init(dest_dir);

  // Initialize CURL
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("Failed to initialize CURL");
  }

  // Initialize the buffer for the response
  std::string response;

  try {
    // Normalize repository URL
    std::string normalized_url = repo_url;
    if (normalized_url.ends_with("/")) {
      normalized_url = normalized_url.substr(0, normalized_url.length() - 1);
    }
    if (!normalized_url.ends_with(".git")) {
      normalized_url += ".git";
    }

    // Get refs URL
    std::string refs_url = normalized_url + "/info/refs?service=git-upload-pack";
    std::cout << "Fetching from: " << refs_url << std::endl;

    // Set up common HTTP headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "User-Agent: git/2.34.1");
    headers = curl_slist_append(headers, "Content-Type: application/x-git-upload-pack-request");

    // Configure CURL request for fetching refs
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, refs_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                       auto* response = static_cast<std::string*>(userdata);
                       response->append(ptr, size * nmemb);
                       return size * nmemb;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Perform request to get refs
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      throw std::runtime_error(std::string("Failed to fetch refs: ") + curl_easy_strerror(res));
    }

    // Parse refs response to get GitRefs
    std::vector<GitRef> refs;
    std::string main_ref_hash;

    {
      std::istringstream stream(response);
      std::string line;

      // Skip the first line which contains service info
      std::getline(stream, line);

      // Process the remaining lines
      while (std::getline(stream, line)) {
        // Skip empty lines or lines that are too short
        if (line.length() < 4) continue;

        // Decode the length prefix (hex)
        unsigned int length = 0;
        std::istringstream ss(line.substr(0, 4));
        ss >> std::hex >> length;

        if (length == 0) continue; // Skip flush packets

        // Parse the line if it's long enough to contain a hash
        if (length >= 44) {
          std::string hash = line.substr(4, 40);
          size_t name_start = line.find(" refs/", 4);
          if (name_start != std::string::npos) {
            std::string ref_name = line.substr(name_start + 1);
            // Find null terminator or end of string
            size_t null_pos = ref_name.find('\0');
            if (null_pos != std::string::npos) {
              ref_name = ref_name.substr(0, null_pos);
            }

            refs.push_back({ref_name, hash});

            // Save main branch hash (HEAD or main or master)
            if (ref_name == "refs/heads/main" || ref_name == "refs/heads/master") {
              main_ref_hash = hash;
            }
          }
        }
      }
    }

    // If no refs were found, throw an error
    if (refs.empty()) {
      throw std::runtime_error("No refs found in the repository");
    }

    // If main branch not found, use the first ref
    if (main_ref_hash.empty() && !refs.empty()) {
      main_ref_hash = refs[0].hash;
    }

    // Clear the response buffer for the next request
    response.clear();

    // Get upload pack URL
    std::string upload_pack_url = normalized_url + "/git-upload-pack";

    // Create a new request for the packfile
    // Format: 0032want <hash>\n00000009done\n
    std::stringstream request_body;
    request_body << "0032want " << main_ref_hash << "\n";
    request_body << "0000";         // Flush packet
    request_body << "0009done\n";   // End negotiation

    std::string request_str = request_body.str();

    // Set up a new request for fetching the packfile
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, upload_pack_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_str.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                       auto* response = static_cast<std::string*>(userdata);
                       response->append(ptr, size * nmemb);
                       return size * nmemb;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Perform the pack request
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      throw std::runtime_error(std::string("Failed to fetch pack: ") + curl_easy_strerror(res));
    }

    // Process the packfile
    process_packfile(response, dest_dir.string());

    // Update the HEAD reference to point to the main branch
    fs::path head_file_path = dest_dir / ".git" / "HEAD";
    std::ofstream head_file(head_file_path);
    if (!head_file) {
      throw std::runtime_error("Failed to update HEAD reference");
    }
    head_file << "ref: refs/heads/main\n";
    head_file.close();

    // Create the refs directory and save the main branch reference
    fs::path refs_dir = dest_dir / ".git" / "refs" / "heads";
    fs::create_directories(refs_dir);

    fs::path main_ref_path = refs_dir / "main";
    std::ofstream main_ref_file(main_ref_path);
    if (!main_ref_file) {
      throw std::runtime_error("Failed to create main branch reference");
    }
    main_ref_file << main_ref_hash << "\n";
    main_ref_file.close();

    // Clean up CURL resources
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    std::cout << "Repository cloned successfully to " << dest_dir << std::endl;
  }
  catch (const std::exception& e) {
    // Clean up CURL and rethrow the exception
    curl_easy_cleanup(curl);
    throw std::runtime_error("Clone failed: " + std::string(e.what()));
  }
}

// This function needs to be fixed to properly read an object
std::string read_object_content(const fs::path& git_dir, const std::string& object_hash) {
  fs::path object_path = git_dir / "objects" / object_hash.substr(0, 2) / object_hash.substr(2);
  if (!fs::exists(object_path)) {
    throw std::runtime_error("Object not found: " + object_hash);
  }

  std::ifstream object_file(object_path, std::ios::binary);
  if (!object_file) {
    throw std::runtime_error("Failed to open object file: " + object_path.string());
  }

  std::string compressed_data((std::istreambuf_iterator<char>(object_file)),
                              std::istreambuf_iterator<char>());
  object_file.close();

  // Decompress the content
  z_stream zStream;
  memset(&zStream, 0, sizeof(zStream));
  zStream.zalloc = Z_NULL;
  zStream.zfree = Z_NULL;
  zStream.opaque = Z_NULL;

  if (inflateInit(&zStream) != Z_OK) {
    throw std::runtime_error("Failed to initialize zlib inflate");
  }

  zStream.avail_in = compressed_data.size();
  zStream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed_data.data()));

  std::string decompressed_content;
  char buffer[4096];

  do {
    zStream.avail_out = sizeof(buffer);
    zStream.next_out = reinterpret_cast<Bytef*>(buffer);

    int ret = inflate(&zStream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zStream);
      throw std::runtime_error("Zlib inflate error: " + std::to_string(ret));
    }

    decompressed_content.append(buffer, sizeof(buffer) - zStream.avail_out);
  } while (zStream.avail_out == 0);

  inflateEnd(&zStream);

  // Find the null byte that separates the header from the content
  size_t null_pos = decompressed_content.find('\0');
  if (null_pos == std::string::npos) {
    throw std::runtime_error("Invalid object format: " + object_hash);
  }

  // Return only the content part (after the null byte)
  return decompressed_content.substr(null_pos + 1);
}

// Function to read an object from a specific path
std::string read_object_content(const std::string& object_hash, const std::string& repo_path) {
  fs::path git_dir = fs::path(repo_path) / ".git";
  return read_object_content(git_dir, object_hash);
}
