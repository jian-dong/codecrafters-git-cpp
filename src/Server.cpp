#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <zlib.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

/**
 * @brief Decompresses zlib-compressed data.
 *
 * @param compressedData The input compressed string.
 * @return The decompressed string.
 * @throws std::runtime_error if decompression fails.
 */
std::string zlib_decompress(const std::string &compressedData) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    // zlib expects a non-const pointer
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressedData.data()));
    zs.avail_in = static_cast<uInt>(compressedData.size());

    int ret;
    constexpr size_t bufferSize = 32768; // 32KB buffer
    char outBuffer[bufferSize];
    std::string decompressed;

    do {
        zs.next_out = reinterpret_cast<Bytef*>(outBuffer);
        zs.avail_out = bufferSize;

        ret = inflate(&zs, 0);
        if (decompressed.size() < zs.total_out) {
            decompressed.append(outBuffer, zs.total_out - decompressed.size());
        }
    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        throw std::runtime_error("inflate failed: " + std::to_string(ret));
    }

    return decompressed;
}

/**
 * @brief Recursively searches for a .git directory in the current or parent directories.
 *
 * @param path The starting path (default is the current path).
 * @return The path to the .git directory, or an empty path if not found.
 */
fs::path find_git_root(fs::path path = fs::current_path()) {
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

/**
 * @brief Reads a Git object file and returns its content (skipping the header).
 *
 * @param gitDir The .git directory.
 * @param objectHash The hash of the object.
 * @return The object content.
 * @throws std::runtime_error if the object is not found or is invalid.
 */
std::string read_object_content(const fs::path &gitDir, const std::string &objectHash) {
    fs::path objectPath = gitDir / "objects" / objectHash.substr(0, 2) / objectHash.substr(2);
    if (!fs::exists(objectPath)) {
        throw std::runtime_error("Object not found: " + objectHash);
    }

    std::ifstream objectFile(objectPath, std::ios::binary);
    if (!objectFile.is_open()) {
        throw std::runtime_error("Failed to open object file: " + objectPath.string());
    }

    std::string compressedData((std::istreambuf_iterator<char>(objectFile)),
                                 std::istreambuf_iterator<char>());
    objectFile.close();

    std::string decompressed = zlib_decompress(compressedData);

    // Skip the header (up to the first '\0')
    size_t nullPos = decompressed.find('\0');
    if (nullPos == std::string::npos) {
        throw std::runtime_error("Invalid object format: " + objectHash);
    }

    return decompressed.substr(nullPos + 1);
}

/**
 * @brief Computes the SHA1 hash of the given data and returns it as a hexadecimal string.
 *
 * @param data The input data.
 * @return The SHA1 hash in hex.
 */
std::string sha_file(const std::string &data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    // Compute SHA1 hash
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

/**
 * @brief Compresses the given data using zlib.
 *
 * @param data The input data to compress.
 * @param bound Pointer to the size of the destination buffer (will be updated with the actual compressed size).
 * @param dest The destination buffer to hold compressed data.
 * @throws std::runtime_error if compression fails.
 */
void compressFile(const std::string &data, uLong *bound, unsigned char *dest) {
    int ret = compress(dest, bound, reinterpret_cast<const Bytef*>(data.c_str()), data.size());
    if (ret != Z_OK) {
        throw std::runtime_error("Compression failed: " + std::to_string(ret));
    }
}

/**
 * @brief Reads a file, constructs a Git blob object, compresses it, writes it to the repository,
 * and returns the computed object hash.
 *
 * @param filePath The path to the file.
 * @return The SHA1 hash of the blob object.
 */
std::string hash_object(const std::string &filePath) {
    // Read file contents
    std::ifstream fileStream(filePath, std::ios::binary);
    if (!fileStream.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }
    std::stringstream buffer;
    buffer << fileStream.rdbuf();
    fileStream.close();
    std::string fileContents = buffer.str();

    // Construct blob object: "blob <size>\0<content>"
    std::string blobContent = "blob " + std::to_string(fileContents.size()) + '\0' + fileContents;

    // Compute SHA1 hash of the blob
    std::string objectHash = sha_file(blobContent);

    // Compress the blob content
    uLong compressBoundSize = compressBound(blobContent.size());
    std::vector<unsigned char> compressedData(compressBoundSize);
    compressFile(blobContent, &compressBoundSize, compressedData.data());

    // Create directory .git/objects/XX where XX are the first two characters of the hash
    std::string dir = ".git/objects/" + objectHash.substr(0, 2);
    fs::create_directories(dir);

    // Construct the object file path
    std::string objectPath = dir + "/" + objectHash.substr(2);
    std::ofstream objectFile(objectPath, std::ios::binary);
    if (!objectFile.is_open()) {
        throw std::runtime_error("Failed to open object file for writing: " + objectPath);
    }
    objectFile.write(reinterpret_cast<const char*>(compressedData.data()), compressBoundSize);
    objectFile.close();

    return objectHash;
}

#include <sstream>
#include <vector>

/**
 * @brief Reads a tree object, parses its entries, and returns the names (sorted) each on一行.
 *
 * @param gitDir The .git directory.
 * @param treeHash The hash of the tree object.
 * @return A string with each entry name followed by a newline.
 */
std::string read_tree_object(const fs::path &gitDir, const std::string &treeHash) {
    fs::path objectPath = gitDir / "objects" / treeHash.substr(0, 2) / treeHash.substr(2);
    if (!fs::exists(objectPath)) {
        throw std::runtime_error("Object not found: " + treeHash);
    }
    std::ifstream objectFile(objectPath, std::ios::binary);
    if (!objectFile.is_open()) {
        throw std::runtime_error("Failed to open object file: " + objectPath.string());
    }
    std::string compressedContent((std::istreambuf_iterator<char>(objectFile)),
                                    std::istreambuf_iterator<char>());
    objectFile.close();

    // Decompress the content
    z_stream zStream;
    zStream.zalloc = Z_NULL;
    zStream.zfree = Z_NULL;
    zStream.opaque = Z_NULL;
    zStream.avail_in = compressedContent.size();
    zStream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressedContent.data()));

    if (inflateInit2(&zStream, 15 + 32) != Z_OK) { // 15 + 32 for gzip and zlib both, auto detect
        throw std::runtime_error("Failed to initialize zlib inflate");
    }

    std::string decompressedContent;
    int ret;
    do {
        char buffer[1024];
        zStream.avail_out = sizeof(buffer);
        zStream.next_out = reinterpret_cast<Bytef*>(buffer);

        ret = inflate(&zStream, Z_SYNC_FLUSH); // Z_SYNC_FLUSH to handle potential errors

        if (ret < 0 && ret != Z_BUF_ERROR && ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zStream);
            throw std::runtime_error("Zlib inflate error: " + std::to_string(ret));
        }

        if (ret == Z_BUF_ERROR || ret == Z_OK || ret == Z_STREAM_END) {
            decompressedContent.append(buffer, sizeof(buffer) - zStream.avail_out);
        }

    } while (ret != Z_STREAM_END && zStream.avail_out == 0);

    inflateEnd(&zStream);

    // Parse the content
    std::istringstream contentStream(decompressedContent);
    std::string type;
    size_t size;
    char nullChar;
    contentStream >> type >> size >> std::noskipws >> nullChar; // Read "tree size\0"
    if (type != "tree" || nullChar != '\0') {
        throw std::runtime_error("Object is not a tree or has invalid format");
    }

    std::string entriesData = decompressedContent.substr(contentStream.tellg());
    std::string result = "";
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos < entriesData.size()) {
        size_t spacePos = entriesData.find(' ', pos);
        if (spacePos == std::string::npos) break;
        size_t nullPos = entriesData.find('\0', spacePos + 1);
        if (nullPos == std::string::npos) break;

        std::string name = entriesData.substr(spacePos + 1, nullPos - (spacePos + 1));
        names.push_back(name);
        pos = nullPos + 21; // Move position to after null byte and 20-byte hash
    }
    std::sort(names.begin(), names.end());
    for(const auto& n : names) {
        result += n + "\n";
    }

    return result;
}

/**
 * @brief Helper: Converts a hex string to a binary string.
 *
 * @param hex The hex string (expected even length).
 * @return A string containing the binary data.
 */
std::string hexToBytes(const std::string &hex) {
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        char byte = static_cast<char>(std::stoi(byteStr, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

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
std::string write_tree(const fs::path &directory) {
    struct TreeEntry {
         std::string mode;
         std::string name;
         std::string hash; // 40-character hex string
    };
    std::vector<TreeEntry> entries;

    for (const auto &entry : fs::directory_iterator(directory)) {
        // Skip .git 目录
        if (entry.path().filename() == ".git")
            continue;

        if (fs::is_directory(entry.path())) {
            std::string treeHash = write_tree(entry.path());
            entries.push_back({"40000", entry.path().filename().string(), treeHash});
        } else if (fs::is_regular_file(entry.path())) {
            std::string blobHash = hash_object(entry.path().string());
            entries.push_back({"100644", entry.path().filename().string(), blobHash});
        }
        // 其他类型（如符号链接）可以按需求处理
    }

    // 按文件名排序
    std::sort(entries.begin(), entries.end(), [](const TreeEntry &a, const TreeEntry &b) {
         return a.name < b.name;
    });

    // 构造 tree body：每个 entry 为 "<mode> <filename>\0" + raw hash(20 字节)
    std::string treeBody;
    for (const auto &e : entries) {
        treeBody += e.mode + " " + e.name + '\0';
        treeBody += hexToBytes(e.hash);
    }

    // 构造完整的 tree 对象内容： header + body
    std::string fullContent = "tree " + std::to_string(treeBody.size()) + '\0' + treeBody;
    std::string treeHash = sha_file(fullContent);

    // 压缩内容并写入 .git/objects
    uLong compressBoundSize = compressBound(fullContent.size());
    std::vector<unsigned char> compressedData(compressBoundSize);
    compressFile(fullContent, &compressBoundSize, compressedData.data());

    fs::path gitDir = find_git_root(fs::current_path());
    if (gitDir.empty()) {
         // 如果没有找到，则默认在当前目录下的 .git 文件夹
         gitDir = fs::current_path() / ".git";
    }
    fs::path objectDir = gitDir / "objects" / treeHash.substr(0, 2);
    fs::create_directories(objectDir);
    fs::path objectPath = objectDir / treeHash.substr(2);
    std::ofstream objectFile(objectPath, std::ios::binary);
    if (!objectFile.is_open()) {
         throw std::runtime_error("Failed to open object file for writing: " + objectPath.string());
    }
    objectFile.write(reinterpret_cast<const char*>(compressedData.data()), compressBoundSize);
    objectFile.close();

    return treeHash;
}

int main(int argc, char *argv[]) {
    // Ensure output is flushed immediately
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2) {
        std::cerr << "No command provided.\n";
        return EXIT_FAILURE;
    }

    std::string command = argv[1];

    if (command == "init") {
        // Initialize a simple Git repository structure
        try {
            fs::create_directories(".git/objects");
            fs::create_directories(".git/refs/heads");

            std::ofstream headFile(".git/HEAD");
            if (!headFile.is_open()) {
                std::cerr << "Failed to create .git/HEAD file.\n";
                return EXIT_FAILURE;
            }
            headFile << "ref: refs/heads/main\n";
            headFile.close();

            std::cout << "Initialized git directory\n";
        } catch (const fs::filesystem_error &e) {
            std::cerr << e.what() << "\n";
            return EXIT_FAILURE;
        }
    } else if (command == "cat-file") {
        // Usage: cat-file -p <object-hash>
        if (argc < 4) {
            std::cerr << "Usage: cat-file -p <object-hash>\n";
            return EXIT_FAILURE;
        }

        std::string option = argv[2];
        std::string objectHash = argv[3];

        if (option != "-p") {
            std::cerr << "Only -p option is supported.\n";
            return EXIT_FAILURE;
        }

        fs::path gitDir = find_git_root();
        if (gitDir.empty()) {
            std::cerr << "Not a git repository (or any of the parent directories)\n";
            return EXIT_FAILURE;
        }

        try {
            std::string content = read_object_content(gitDir, objectHash);
            std::cout << content;
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    } else if (command == "hash-object") {
        // Usage: hash-object -w <file>
        if (argc < 4) {
            std::cerr << "Usage: hash-object -w <file>\n";
            return EXIT_FAILURE;
        }

        std::string option = argv[2];
        std::string file = argv[3];

        if (option != "-w") {
            std::cerr << "Only -w option is supported.\n";
            return EXIT_FAILURE;
        }

        try {
            std::string objectHash = hash_object(file);
            std::cout << objectHash << "\n";
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    } else if (command == "ls-tree") {
        // Usage: ls-tree --name-only <tree-hash>
        if (argc < 4) {
            std::cerr << "Usage: ls-tree --name-only <tree-hash>\n";
            return EXIT_FAILURE;
        }

        std::string option = argv[2];
        std::string treeHash = argv[3];

        if (option != "--name-only") {
            std::cerr << "Only --name-only option is supported.\n";
            return EXIT_FAILURE;
        }

        fs::path gitDir = find_git_root();
        if (gitDir.empty()) {
            std::cerr << "Not a git repository (or any of the parent directories)\n";
            return EXIT_FAILURE;
        }
        // 使用正确的解析函数
        try {
            std::string treeContent = read_tree_object(gitDir, treeHash);
            std::cout << treeContent;
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    } else if (command == "write-tree") {
        // Usage: write-tree
        try {
            // 以当前目录为工作树（自动跳过 .git 目录）
            std::string treeHash = write_tree(fs::current_path());
            std::cout << treeHash << "\n";
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    } else {
        std::cerr << "Unknown command " << command << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
