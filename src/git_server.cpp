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
  std::string object_hash = compute_sha1(blob_content);

  // Compress the blob content
  uLong compress_bound_size = compressBound(blob_content.size());
  std::vector<unsigned char> compressed_data(compress_bound_size);
  zlib_compress(blob_content, &compress_bound_size, compressed_data.data());

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
  std::string tree_hash = compute_sha1(full_content);

  // 压缩内容并写入 .git/objects
  uLong compress_bound_size = compressBound(full_content.size());
  std::vector<unsigned char> compressedData(compress_bound_size);
  zlib_compress(full_content, &compress_bound_size, compressedData.data());

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
  std::string commit_hash = compute_sha1(full_content);

  // Create object directory
  fs::path object_dir = git_dir / "objects" / commit_hash.substr(0, 2);
  fs::create_directories(object_dir);
  uLong compress_bound_size = compressBound(full_content.size());
  std::vector<unsigned char> compressed_data(compress_bound_size);
  zlib_compress(full_content, &compress_bound_size, compressed_data.data());

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

// curl helper function
size_t pack_data_callback(void* received_data, size_t element_size, size_t num_element,
                          void* userdata) {
  auto* accumulated_data = (std::string*)userdata;
  *accumulated_data += std::string(static_cast<char*>(received_data), num_element);

  return element_size * num_element;
}

// curl helper function
size_t write_callback(void* received_data, size_t element_size, size_t num_element,
                      void* userdata) {
  size_t total_size = element_size * num_element;
  std::string received_text((char*)received_data, num_element);

  std::string* master_hash = (std::string*)userdata;
  if (received_text.find("servie=git-upload-pack") == std::string::npos) {
    size_t hash_pos = received_text.find("refs/heads/master\n");
    if (hash_pos != std::string::npos) {
      *master_hash = received_text.substr(hash_pos - 41, 40);
    }
  }

  return total_size;
}

std::pair<std::string, std::string> curl_request(const std::string& url) {
  CURL* handle = curl_easy_init();
  if (handle) {
    // fetch info/refs
    curl_easy_setopt(handle, CURLOPT_URL, (url + "/info/refs?service=git-upload-pack").c_str());

    std::string packhash;
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*)&packhash);
    curl_easy_perform(handle);
    curl_easy_reset(handle);

    // fetch git-upload-pack
    curl_easy_setopt(handle, CURLOPT_URL, (url + "/git-upload-pack").c_str());
    std::string postdata = "0032want " + packhash + "\n" + "00000009done\n";
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, postdata.c_str());

    std::string pack;
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, static_cast<void*>(&pack));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, pack_data_callback);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-git-upload-pack-request");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_perform(handle);

    // clean up
    curl_easy_cleanup(handle);
    curl_slist_free_all(headers);

    return {pack, packhash};
  } else {
    std::cerr << "Failed to initialize curl.\n";
    return {};
  }
}

int read_length(const std::string& pack, size_t* pos) {
  int length = 0;

  // extract the lower 4 bits of the first byte
  length |= pack[*pos] & 0x0F;

  // if the MSB is set, read the next byte
  if (pack[*pos] & 0x80) {
    (*pos)++;

    while (pack[*pos] & 0x80) {
      length <<= 7;
      length |= pack[*pos] & 0x7F;
      (*pos)++;
    }

    // read the last byte
    length <<= 7;
    length |= pack[*pos];
  }

  (*pos)++;  // move to the next position

  return length;
}

std::string apply_delta(const std::string& delta_contents, const std::string& base_contents) {
  std::string reconstructed_object;
  size_t current_position_in_delta = 0;

  // read and skip the length of the base object
  read_length(delta_contents, &current_position_in_delta);
  read_length(delta_contents, &current_position_in_delta);

  // iterate through the delta contents
  while (current_position_in_delta < delta_contents.length()) {
    unsigned char current_instruction = delta_contents[current_position_in_delta++];

    // check if the highest bit of the instruction byte is set
    if (current_instruction & 0x80) {
      int copy_offset = 0;
      int copy_size = 0;
      int bytes_processed_for_offset = 0;

      // calculate copy offset from the delta contents
      for (int i = 3; i >= 0; i--) {
        copy_offset <<= 8;
        if (current_instruction & (1 << i)) {
          copy_offset += static_cast<unsigned char>(delta_contents[current_position_in_delta + i]);
          bytes_processed_for_offset++;
        }
      }

      int bytes_processed_for_size = 0;
      // calculate copy size from the delta contents
      for (int i = 6; i >= 4; i--) {
        copy_size <<= 8;
        if (current_instruction & (1 << i)) {
          copy_size += static_cast<unsigned char>(
              delta_contents[current_position_in_delta + i - (4 - bytes_processed_for_offset)]);
          bytes_processed_for_size++;
        }
      }

      // default size to 0x100000 if no size was specified
      if (copy_size == 0) {
        copy_size = 0x100000;
      }

      // append the copied data from base contents to the reconstructed object
      reconstructed_object += base_contents.substr(copy_offset, copy_size);
      current_position_in_delta += bytes_processed_for_size + bytes_processed_for_offset;
    } else {
      // direct add insturction, the highest bit is not set
      int add_size = current_instruction & 0x7F;
      reconstructed_object += delta_contents.substr(current_position_in_delta, add_size);
      current_position_in_delta += add_size;
    }
  }

  return reconstructed_object;
}

void compress_and_store(const std::string& hash, const std::string& content,
                        std::string dir = ".") {
  FILE* input = fmemopen((void*)content.c_str(), content.length(), "rb");
  std::string hash_folder = hash.substr(0, 2);
  std::string object_path = dir + "/.git/objects/" + hash_folder + '/';
  if (!std::filesystem::exists(object_path)) {
    std::filesystem::create_directories(object_path);
  }

  std::string object_file_path = object_path + hash.substr(2);
  if (!std::filesystem::exists(object_file_path)) {
    FILE* output = fopen(object_file_path.c_str(), "wb");
    if (!zlib_compress_file(input, output)) {
      std::cerr << "Failed to compress data.\n";
      return;
    }
    fclose(output);
  }

  fclose(input);
}

int cat_file_for_clone(const char* file_path, const std::string& dir, FILE* dest,
                       bool print_out = false) {
  try {
    std::string blob_sha = file_path;
    std::string blob_path = dir + "/.git/objects/" + blob_sha.insert(2, "/");
    if (print_out) std::cout << "blob path: " << blob_path << std::endl;

    FILE* blob_file = fopen(blob_path.c_str(), "rb");
    if (blob_file == NULL) {
      std::cerr << "Invalid object hash.\n";
      return EXIT_FAILURE;
    }

    zlib_decompress_file(blob_file, dest);
    fclose(blob_file);
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

void restore_tree(const std::string& tree_hash, const std::string& dir,
                  const std::string& proj_dir) {
  // construct the path to the tree object
  std::string object_path =
      proj_dir + "/.git/objects/" + tree_hash.substr(0, 2) + '/' + tree_hash.substr(2);
  std::ifstream master_tree(object_path);

  // read the contents of the tree object into a buffer
  std::ostringstream buffer;
  buffer << master_tree.rdbuf();

  // decompress the tree object
  std::string tree_contents = zlib_decompress_string(buffer.str());

  // skip the metadata part of the tree object
  tree_contents = tree_contents.substr(tree_contents.find('\0') + 1);

  // iterate over each entry in the tree object
  size_t pos = 0;
  while (pos < tree_contents.length()) {
    if (tree_contents.find("40000", pos) == pos) {
      pos += 6;  // skip the mode 40000

      // extract the directory path
      std::string path = tree_contents.substr(pos, tree_contents.find('\0', pos) - pos);
      pos += path.length() + 1;  // skip the path and the null byte

      // extract the hash of the nested tree object
      std::string next_hash = hash_to_hex(tree_contents.substr(pos, 20));

      // create directories and recursively restore the nested tree
      std::filesystem::create_directory(dir + '/' + path);
      restore_tree(next_hash, dir + '/' + path, proj_dir);
      pos += 20;  // skip the hash
    } else {
      pos += 7;  // skip the mode 100644

      // extract the file path
      std::string path = tree_contents.substr(pos, tree_contents.find('\0', pos) - pos);
      pos += path.length() + 1;  // skip the path and the null byte

      // extract the hash of the blob object
      std::string blob_hash = hash_to_hex(tree_contents.substr(pos, 20));

      // create the file and write its contents
      FILE* new_file = fopen((dir + '/' + path).c_str(), "wb");
      cat_file_for_clone(blob_hash.c_str(), proj_dir, new_file);
      fclose(new_file);
      pos += 20;  // skip the hash
    }
  }
}

void handle_git_clone(const std::string& repo_url, const fs::path& dest_dir) {
  // Create the destination directory if it doesn't exist
  fs::create_directories(dest_dir);

  // Initialize git repository in the destination directory
  handle_git_init(dest_dir);
  // fetch the repository
  auto [pack, pack_hash] = curl_request(repo_url);

  // parse the pack file
  int num_objects = 0;
  for (int i = 16; i < 20; i++) {
    num_objects = num_objects << 8;
    num_objects = num_objects | (unsigned char)pack[i];
  }
  pack = pack.substr(20, pack.length() - 40);  // removing the headers of HTTP

  // proecessing object files in a git pack file
  int object_type;
  size_t current_position = 0;
  std::string master_commit_contents;
  for (int object_index = 0; object_index < num_objects; object_index++) {
    // extract object type from the first byte
    object_type = (pack[current_position] & 112) >> 4;  // 112 is 11100000

    // read the object's length
    size_t object_length = read_length(pack, &current_position);
    (void)object_length;

    // process based on object type
    if (object_type == 6) {  // offset deltas: ignore it
      throw std::invalid_argument("Offset deltas not implemented.\n");
    } else if (object_type == 7) {  // reference deltas
      // process reference deltas
      std::string digest = pack.substr(current_position, 20);
      std::string hash = hash_to_hex(digest);
      current_position += 20;

      // read the base object's contents
      std::ifstream file(dest_dir.filename().string() + "/.git/objects/" + hash.insert(2, "/"));
      std::stringstream buffer;
      buffer << file.rdbuf();
      std::string file_contents = buffer.str();

      std::string base_object_contents = zlib_decompress_string(file_contents);

      // extract and remove the object type
      std::string object_type_extracted =
          base_object_contents.substr(0, base_object_contents.find(" "));
      base_object_contents = base_object_contents.substr(base_object_contents.find('\0') + 1);

      // apply delta to base object
      std::string delta_contents = zlib_decompress_string(pack.substr(current_position));
      std::string reconstructed_contents = apply_delta(delta_contents, base_object_contents);

      // reconstruct the object with its type and length
      reconstructed_contents = object_type_extracted + ' ' +
                               std::to_string(reconstructed_contents.length()) + '\0' +
                               reconstructed_contents;

      // compute the object hash and store it
      std::string object_hash = compute_sha1(reconstructed_contents);
      compress_and_store(object_hash.c_str(), reconstructed_contents, dest_dir.string());

      // advance position past the delta data
      std::string compressed_delta = zlib_compress_string(delta_contents);
      current_position += compressed_delta.length();

      // update master commits if hash matches
      if (hash.compare(pack_hash) == 0) {
        master_commit_contents = reconstructed_contents.substr(reconstructed_contents.find('\0'));
      }
    } else {  // other object types (1: commit, 2: tree, other: blob)
      // process standard objects
      std::string object_contents = zlib_decompress_string(pack.substr(current_position));
      current_position += zlib_compress_string(object_contents).length();

      // prepare object header
      std::string object_type_str = (object_type == 1)   ? "commit "
                                    : (object_type == 2) ? "tree "
                                                         : "blob ";
      object_contents =
          object_type_str + std::to_string(object_contents.length()) + '\0' + object_contents;

      // store the object and update master commits if hash matches
      std::string object_hash = compute_sha1(object_contents);
      std::string compressed_object = zlib_compress_string(object_contents);
      compress_and_store(object_hash.c_str(), object_contents, dest_dir.string());
      if (object_hash.compare(pack_hash) == 0) {
        master_commit_contents = object_contents.substr(object_contents.find('\0'));
      }
    }
  }

  // restore the tree
  std::string tree_hash =
      master_commit_contents.substr(master_commit_contents.find("tree") + 5, 40);
  restore_tree(tree_hash, dest_dir.string(), dest_dir.string());
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
