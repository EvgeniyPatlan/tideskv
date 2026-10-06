#pragma once
#include <filesystem>
#include <random>
#include <string>

namespace tideskv::test {

// Creates a unique empty directory; removes it recursively on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag = "tideskv") {
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() /
            (tag + "-" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace tideskv::test
