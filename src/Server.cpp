#include <iostream>
#include <fstream>
#include <string>
#include <zlib.h> // 引入 zlib 库
#include <filesystem> // C++17 文件系统库
#include <sstream>
#include <cstring>
#include <openssl/sha.h>

namespace fs = std::filesystem;

// 解压缩函数 (与之前相同)
std::string zlib_decompress(const std::string& compressed_data) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    zs.next_in = (Bytef*)compressed_data.data();
    zs.avail_in = compressed_data.size();

    int ret;
    char outbuffer[32768]; // 32KB buffer
    std::string decompressed_data;

    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = inflate(&zs, 0);

        if (decompressed_data.size() < zs.total_out) {
            decompressed_data.append(outbuffer, zs.total_out - decompressed_data.size());
        }
    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        throw std::runtime_error("inflate failed: " + std::to_string(ret));
    }

    return decompressed_data;
}

// 查找 .git 目录 (与之前相同)
fs::path find_git_root(fs::path path = fs::current_path()) {
    while (!path.empty()) {
        if (fs::exists(path / ".git") && fs::is_directory(path / ".git")) {
            return path / ".git";
        }
        if (path.has_parent_path()) {
            path = path.parent_path();
        } else {
            return ""; // Not a Git repository
        }
    }
    return "";
}

// 读取并解析 Git 对象 (简化版，只返回内容)
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

    std::string decompressed_data = zlib_decompress(compressed_data);

    // 找到内容开始的位置（跳过头部）
    size_t null_pos = decompressed_data.find('\0');
    if (null_pos == std::string::npos) {
        throw std::runtime_error("Invalid object format: " + object_hash);
    }

    return decompressed_data.substr(null_pos + 1); // 只返回内容
}


// 计算 Git blob 对象的 SHA1 哈希值
std::string hash_object(const std::string& content) {
    // 构造 header: "blob <size>\0"
    std::string header = "blob " + std::to_string(content.size());
    header.push_back('\0');  // 添加空字符作为分隔符

    // 拼接 header 与内容
    std::string store = header + content;

    // 计算 SHA1 哈希值
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(store.data()), store.size(), hash);

    // 将二进制哈希转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(hash[i]);
    }
    return ss.str();
}


int main(int argc, char *argv[]) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  if (argc < 2) {
    std::cerr << "No command provided.\n";
    return EXIT_FAILURE;
  }

  std::string command = argv[1];

  if (command == "init") {
    try {
      std::filesystem::create_directory(".git");
      std::filesystem::create_directory(".git/objects");
      std::filesystem::create_directory(".git/refs");
      std::filesystem::create_directory(".git/refs/heads"); // 创建 heads 目录


      std::ofstream headFile(".git/HEAD");
      if (headFile.is_open()) {
        headFile << "ref: refs/heads/main\n";
        headFile.close();
      } else {
        std::cerr << "Failed to create .git/HEAD file.\n";
        return EXIT_FAILURE;
      }

      std::cout << "Initialized git directory\n";
    } catch (const std::filesystem::filesystem_error &e) {
      std::cerr << e.what() << '\n';
      return EXIT_FAILURE;
    }
  } else if (command == "cat-file") {
        if (argc < 4) {
            std::cerr << "Usage: cat-file -p <object-hash>\n";
            return EXIT_FAILURE;
        }

        std::string option = argv[2];
        std::string object_hash = argv[3];

        if (option != "-p") {
            std::cerr << "Only -p option is supported.\n";
            return EXIT_FAILURE;
        }

        fs::path git_dir = find_git_root();
        if (git_dir.empty()) {
            std::cerr << "Not a git repository (or any of the parent directories)\n";
            return EXIT_FAILURE;
        }

        try {
            std::string content = read_object_content(git_dir, object_hash);
            std::cout << content; // 直接输出内容
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }

  } else if (command == "hash-object") {
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

    std::ifstream fileStream(file, std::ios::binary);
    if (!fileStream.is_open()) {
      std::cerr << "Failed to open file: " << file << '\n';
      return EXIT_FAILURE;
    }

    std::string content((std::istreambuf_iterator<char>(fileStream)),
                        std::istreambuf_iterator<char>());
    fileStream.close();

    std::string object_hash = hash_object(content);
    std::cout << object_hash << '\n';
  } else {
    std::cerr << "Unknown command " << command << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}