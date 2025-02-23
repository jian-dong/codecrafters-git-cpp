#include "git-cpp/git_server.hpp"
#include <algorithm>

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

std::string read_object_content(const fs::path& git_dir, const std::string& object_hash) {
  fs::path object_path = git_dir / "objects" / object_hash.substr(0, 2) / object_hash.substr(2);
  if (!fs::exists(object_path)) {
    throw std::runtime_error("Object not found: " + object_hash);
  }

  std::ifstream object_file(object_path, std::ios::binary);
  if (!object_file.is_open()) {
    throw std::runtime_error("Failed to open object file: " + object_path.string());
  }

  std::string compressed_data((std::istreambuf_iterator<char>(object_file)),
                              std::istreambuf_iterator<char>());
  object_file.close();

  std::string decompressed = zlib_decompress(compressed_data);

  // Skip the header (up to the first '\0')
  size_t nullPos = decompressed.find('\0');
  if (nullPos == std::string::npos) {
    throw std::runtime_error("Invalid object format: " + object_hash);
  }

  return decompressed.substr(nullPos + 1);
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
  std::vector<unsigned char> compressedData(compress_bound_size);
  zlib_compress_file(blob_content, &compress_bound_size, compressedData.data());

  // Create directory .git/objects/XX where XX are the first two characters of the hash
  std::string dir = ".git/objects/" + object_hash.substr(0, 2);
  fs::create_directories(dir);

  // Construct the object file path
  std::string object_path = dir + "/" + object_hash.substr(2);
  std::ofstream object_file(object_path, std::ios::binary);
  if (!object_file.is_open()) {
    throw std::runtime_error("Failed to open object file for writing: " + object_path);
  }
  object_file.write(reinterpret_cast<const char*>(compressedData.data()), compress_bound_size);
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
    const std::string author = "John Doe <john@example.com>";
    const std::string committer = author;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()
    );

    // Build commit content with correct format
    std::string commit_content = "tree " + tree_hash + "\n";

    if (!parent_hash.empty()) {
        commit_content += "parent " + parent_hash + "\n";
    }

    commit_content += "author " + author + " " + timestamp + " -0800\n" +
                     "committer " + committer + " " + timestamp + " -0800\n" +
                     "\n" + commit_message + "\n";

    // Prepare the content with Git object header
    std::string content = "commit " + std::to_string(commit_content.length()) + "\0" + commit_content;

    // Calculate SHA1 hash
    unsigned char hash[20];
    SHA1(reinterpret_cast<const unsigned char*>(content.c_str()), content.length(), hash);
    std::string hash_str = bytes_to_hex_string(hash, 20);

    // Create object directory if it doesn't exist
    fs::path object_dir = git_dir / "objects" / hash_str.substr(0, 2);
    fs::create_directories(object_dir);

    // Open file for writing
    fs::path object_path = object_dir / hash_str.substr(2);
    std::ofstream object_file(object_path, std::ios::binary);
    if (!object_file) {
        throw std::runtime_error("Failed to create commit object file");
    }

    // Initialize zlib stream
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;

    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib");
    }

    // Compress and write content
    zs.next_in = (Bytef*)content.data();
    zs.avail_in = content.length();

    char outbuffer[8192];
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        if (deflate(&zs, Z_FINISH) == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            throw std::runtime_error("Failed to compress commit object");
        }

        object_file.write(outbuffer, sizeof(outbuffer) - zs.avail_out);
    } while (zs.avail_out == 0);

    // Clean up
    deflateEnd(&zs);
    object_file.close();

    // Output the hash of the new commit
    std::cout << hash_str << "\n";
}
