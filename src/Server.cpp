#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // You can use print statements as follows for debugging, they'll be visible
  // when running tests.

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
    if (argc < 3) {
      std::cerr << "No object hash provided.\n";
      return EXIT_FAILURE;
    }

    std::string objectHash = argv[2];
    std::string objectPath = ".git/objects/" + objectHash.substr(0, 2) + "/" +
                             objectHash.substr(2, objectHash.length() - 2);

    std::ifstream objectFile(objectPath, std::ios::binary);
    if (objectFile.is_open()) {
      std::string objectContent((std::istreambuf_iterator<char>(objectFile)),
                                std::istreambuf_iterator<char>());
      std::cout << objectContent;
      objectFile.close();
    } else {
      std::cerr << "Failed to open object file.\n";
      return EXIT_FAILURE;
    }
  }
  else {
    std::cerr << "Unknown command " << command << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
