#include "git-cpp/git_cli_application.hpp"

#include "CLI/CLI.hpp"
#include "git-cpp/git_remote_client.hpp"
#include "git-cpp/git_repository.hpp"
#include "git-cpp/git_result.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace gitcpp {

namespace {

class CommandRouter {
 public:
  using Handler = std::function<GitStatus()>;

  void Register(CLI::App* command, Handler handler) {
    entries_.push_back({command, std::move(handler)});
  }

  GitStatus Dispatch() const {
    for (const auto& entry : entries_) {
      if (*entry.command) {
        return entry.handler();
      }
    }
    return UnexpectedError(GitErrorCode::kInternalError, "No subcommand selected");
  }

 private:
  struct Entry {
    CLI::App* command;
    Handler handler;
  };

  std::vector<Entry> entries_;
};

int PrintError(const GitError& error) {
  std::cerr << "Error[" << GitErrorCodeName(error.code) << "]: " << error.message
            << '\n';
  if (!error.operation.empty()) {
    std::cerr << "  operation: " << error.operation << '\n';
  }
  if (!error.resource.empty()) {
    std::cerr << "  resource: " << error.resource << '\n';
  }
  if (error.native_code.has_value()) {
    std::cerr << "  native_code: " << *error.native_code;
    if (!error.native_message.empty()) {
      std::cerr << " (" << error.native_message << ")";
    }
    std::cerr << '\n';
  }
  for (const auto& context : error.context) {
    std::cerr << "  at: " << context << '\n';
  }
  return EXIT_FAILURE;
}

GitStatus AsStatus(GitError error) {
  return GitStatus(UnexpectedError(std::move(error)));
}

template <typename T>
GitExpected<T> AttachContext(GitExpected<T> result, std::string context) {
  if (!result) {
    return UnexpectedError(WithContext(std::move(result.error()), std::move(context)));
  }
  return result;
}

template <typename Fn>
GitStatus WithRepository(std::string command_name, Fn&& action) {
  auto repository = AttachContext(GitRepository::Open(),
                                  command_name + ": open repository");
  if (!repository) {
    return AsStatus(std::move(repository.error()));
  }
  return std::forward<Fn>(action)(*repository);
}

}  // namespace

class GitCliApplication::Impl {
 public:
  Impl() : app_("Simple Git Implementation") {
    app_.require_subcommand(1);
    ConfigureCommands();
  }

  int Run(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
      app_.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
      return app_.exit(error);
    }

    auto status = router_.Dispatch();
    if (!status) {
      return PrintError(status.error());
    }
    return EXIT_SUCCESS;
  }

 private:
  void ConfigureCommands() {
    init_command_ = app_.add_subcommand("init", "Initialize a git repository");

    cat_file_command_ = app_.add_subcommand("cat-file", "Display object content");
    cat_file_command_->add_flag("-p", "Pretty-print object content")->required();
    cat_file_command_->add_option("object", object_hash_, "Object hash")->required();

    hash_object_command_ = app_.add_subcommand("hash-object", "Compute object hash");
    hash_object_command_->add_flag("-w", "Write object to database")->required();
    hash_object_command_->add_option("file", file_path_, "File to hash")->required();

    ls_tree_command_ = app_.add_subcommand("ls-tree", "List tree contents");
    ls_tree_command_->add_flag("--name-only", "Show only filenames")->required();
    ls_tree_command_->add_option("tree", tree_hash_, "Tree hash")->required();

    write_tree_command_ = app_.add_subcommand("write-tree", "Create a tree object");

    commit_tree_command_ = app_.add_subcommand("commit-tree", "Create a commit object");
    commit_tree_command_->add_option("tree", tree_hash_, "Tree hash")->required();
    commit_tree_command_->add_option("-p", parent_hash_, "Parent commit hash");
    commit_tree_command_->add_option("-m", commit_message_, "Commit message")->required();

    clone_command_ = app_.add_subcommand("clone", "Clone a remote repository");
    clone_command_->add_option("url", repo_url_, "Repository URL")->required();
    clone_command_->add_option("dest", dest_dir_, "Destination directory")->required();

    router_.Register(init_command_, [this] { return HandleInit(); });
    router_.Register(cat_file_command_, [this] { return HandleCatFile(); });
    router_.Register(hash_object_command_, [this] { return HandleHashObject(); });
    router_.Register(ls_tree_command_, [this] { return HandleLsTree(); });
    router_.Register(write_tree_command_, [this] { return HandleWriteTree(); });
    router_.Register(commit_tree_command_, [this] { return HandleCommitTree(); });
    router_.Register(clone_command_, [this] { return HandleClone(); });
  }

  GitStatus HandleInit() {
    auto repository = AttachContext(GitRepository::Init(fs::current_path()), "init");
    if (!repository) {
      return AsStatus(std::move(repository.error()));
    }
    std::cout << "Initialized git directory\n";
    return {};
  }

  GitStatus HandleCatFile() {
    return WithRepository("cat-file", [this](const GitRepository& repository) {
      auto content =
          AttachContext(repository.ReadObject(object_hash_), "cat-file: read object");
      if (!content) {
        return AsStatus(std::move(content.error()));
      }
      std::cout << *content;
      return GitStatus{};
    });
  }

  GitStatus HandleHashObject() {
    return WithRepository("hash-object", [this](const GitRepository& repository) {
      auto hash_result =
          AttachContext(repository.HashObject(file_path_), "hash-object: hash file");
      if (!hash_result) {
        return AsStatus(std::move(hash_result.error()));
      }
      std::cout << *hash_result << '\n';
      return GitStatus{};
    });
  }

  GitStatus HandleLsTree() {
    return WithRepository("ls-tree", [this](const GitRepository& repository) {
      auto names = AttachContext(repository.ListTreeNames(tree_hash_),
                                 "ls-tree: list tree names");
      if (!names) {
        return AsStatus(std::move(names.error()));
      }
      for (const auto& name : *names) {
        std::cout << name << '\n';
      }
      return GitStatus{};
    });
  }

  GitStatus HandleWriteTree() {
    return WithRepository("write-tree", [this](const GitRepository& repository) {
      auto hash_result = AttachContext(repository.WriteTree(fs::current_path()),
                                       "write-tree: build tree");
      if (!hash_result) {
        return AsStatus(std::move(hash_result.error()));
      }
      std::cout << *hash_result << '\n';
      return GitStatus{};
    });
  }

  GitStatus HandleCommitTree() {
    return WithRepository("commit-tree", [this](const GitRepository& repository) {
      auto commit_hash = AttachContext(
          repository.CommitTree(tree_hash_, parent_hash_, commit_message_),
          "commit-tree: write commit");
      if (!commit_hash) {
        return AsStatus(std::move(commit_hash.error()));
      }
      std::cout << *commit_hash << '\n';
      return GitStatus{};
    });
  }

  GitStatus HandleClone() {
    GitRemoteClient remote_client;
    auto status = AttachContext(remote_client.Clone(repo_url_, dest_dir_), "clone");
    if (!status) {
      return AsStatus(std::move(status.error()));
    }
    return {};
  }

  CLI::App app_;
  CommandRouter router_;

  CLI::App* init_command_ = nullptr;
  CLI::App* cat_file_command_ = nullptr;
  CLI::App* hash_object_command_ = nullptr;
  CLI::App* ls_tree_command_ = nullptr;
  CLI::App* write_tree_command_ = nullptr;
  CLI::App* commit_tree_command_ = nullptr;
  CLI::App* clone_command_ = nullptr;

  std::string object_hash_;
  std::string file_path_;
  std::string tree_hash_;
  std::string parent_hash_;
  std::string commit_message_;
  std::string repo_url_;
  std::string dest_dir_;
};

GitCliApplication::GitCliApplication() : impl_(std::make_unique<Impl>()) {}

GitCliApplication::~GitCliApplication() = default;

GitCliApplication::GitCliApplication(GitCliApplication&&) noexcept = default;

GitCliApplication& GitCliApplication::operator=(GitCliApplication&&) noexcept = default;

int GitCliApplication::Run(int argc, char* argv[]) {
  return impl_->Run(argc, argv);
}

}  // namespace gitcpp
