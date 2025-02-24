#include "git-cpp/CLI11.hpp"
#include "git-cpp/git_server.hpp"

int main(int argc, char* argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  CLI::App app{"Simple Git Implementation"};
  app.require_subcommand(1);

  // init
  app.add_subcommand("init", "Initialize a git repository")->callback([]() {
    handle_git_init(fs::current_path());
  });

  // cat-file -p <hash>
  auto cat_file = app.add_subcommand("cat-file", "Display object content");
  std::string object_hash;
  cat_file->add_flag("-p", "Pretty-print object content")->required();
  cat_file->add_option("object", object_hash, "Object hash")->required();
  cat_file->callback([&]() {
    auto git_dir = find_git_root();
    if (git_dir.empty()) {
      throw CLI::ParseError("Not a git repository", EXIT_FAILURE);
    }
    handle_git_cat_file(git_dir, object_hash);
  });

  // hash-object -w <file>
  auto hash_object = app.add_subcommand("hash-object", "Compute object hash");
  std::string file_path;
  hash_object->add_flag("-w", "Write object to database")->required();
  hash_object->add_option("file", file_path, "File to hash")->required();
  hash_object->callback([&]() { handle_git_hash_object(file_path); });

  // ls-tree --name-only <hash>
  auto ls_tree = app.add_subcommand("ls-tree", "List tree contents");
  std::string tree_hash;
  ls_tree->add_flag("--name-only", "Show only filenames")->required();
  ls_tree->add_option("tree", tree_hash, "Tree hash")->required();
  ls_tree->callback([&]() {
    auto git_dir = find_git_root();
    if (git_dir.empty()) {
      throw CLI::ParseError("Not a git repository", EXIT_FAILURE);
    }
    handle_git_ls_tree(git_dir, tree_hash);
  });

  // write-tree
  app.add_subcommand("write-tree", "Create a tree object")->callback([]() {
    handle_git_write_tree(fs::current_path());
  });

  // commit-tree <tree> -p <parent> -m <message>
  auto commit_tree = app.add_subcommand("commit-tree", "Create a commit object");
  std::string parent_hash;
  std::string commit_message;

  commit_tree->add_option("tree", tree_hash, "Tree hash")->required();
  commit_tree->add_option("-p", parent_hash, "Parent commit hash");
  commit_tree->add_option("-m", commit_message, "Commit message")->required();

  commit_tree->callback([&]() {
    auto git_dir = find_git_root();
    if (git_dir.empty()) {
      throw CLI::ParseError("Not a git repository", EXIT_FAILURE);
    }
    handle_git_commit_tree(git_dir, tree_hash, parent_hash, commit_message);
  });
  // /path/to/your_program.sh clone https://github.com/blah/blah <some_dir>
  // clone <url> <dest_dir>
  std::string repo_url;
  std::string dest_dir;
  auto clone = app.add_subcommand("clone", "Clone a remote repository");
  clone->add_option("url", repo_url, "Repository URL")->required();
  clone->add_option("dest", dest_dir, "Destination directory")->required();
  clone->callback([&]() { handle_git_clone(repo_url, dest_dir); });
  CLI11_PARSE(app, argc, argv);
  return EXIT_SUCCESS;
}
