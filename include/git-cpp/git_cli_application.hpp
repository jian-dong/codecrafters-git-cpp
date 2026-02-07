#pragma once

#include <memory>

namespace gitcpp {

class GitCliApplication {
 public:
  GitCliApplication();
  ~GitCliApplication();

  GitCliApplication(const GitCliApplication&) = delete;
  GitCliApplication& operator=(const GitCliApplication&) = delete;
  GitCliApplication(GitCliApplication&&) noexcept;
  GitCliApplication& operator=(GitCliApplication&&) noexcept;

  int Run(int argc, char* argv[]);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gitcpp
