#include "git-cpp/git_cli_application.hpp"

int main(int argc, char* argv[]) {
  gitcpp::GitCliApplication app;
  return app.Run(argc, argv);
}
