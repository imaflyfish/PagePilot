#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <pagepilot/node_tools.hpp>
#include <span>
namespace pagepilot {
class FileStream {
public:
  explicit FileStream(int value = -1) : value_(value) {}
  ~FileStream();
  FileStream(FileStream &&other) noexcept;
  FileStream &operator=(FileStream &&other) noexcept;
  FileStream(const FileStream &) = delete;
  FileStream &operator=(const FileStream &) = delete;
  int get() const { return value_; }

private:
  int value_;
};
struct UploadEntry {
  std::filesystem::path path;
  std::uint64_t device, inode, size;
  std::array<std::int64_t, 4> times;
  bool directory = false;
  FileStream handle;
};
class CaptureSink {
public:
  CaptureSink(std::filesystem::path path, FileStream parent)
      : path_(std::move(path)), parent_(std::move(parent)) {}
  std::string commit(std::span<const std::uint8_t> bytes);

private:
  std::filesystem::path path_;
  FileStream parent_;
};
class PathGuard {
public:
  explicit PathGuard(std::vector<std::filesystem::path> roots = defaults());
  static std::vector<std::filesystem::path> defaults();
  UploadEntry upload(const std::string &path,
                     bool allow_directory = false) const;
  std::vector<UploadEntry> directory_files(const UploadEntry &folder) const;
  void verify(const UploadEntry &file) const;
  CaptureSink output(const std::string &path) const;

private:
  std::filesystem::path resolve(const std::string &path) const;
  FileStream parent(const std::filesystem::path &path, bool create) const;
  struct Root {
    std::filesystem::path path;
    FileStream descriptor;
  };
  std::vector<Root> roots_;
};
struct PngImage {
  std::vector<std::uint8_t> bytes;
  unsigned width, height;
};
PngImage decode_png(const std::string &base64);
class FileTools {
public:
  FileTools(BrowserSession &browser, PathGuard &paths, MsDuration timeout)
      : browser_(browser), paths_(paths), clock_(timeout) {}
  JsonDoc upload(const JsonDoc &arguments);
  JsonDoc screenshot(const JsonDoc &arguments);

private:
  BrowserSession &browser_;
  PathGuard &paths_;
  StepClock clock_;
};
} // namespace pagepilot
