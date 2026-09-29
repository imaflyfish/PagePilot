#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pagepilot/file_tools.hpp>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
namespace pagepilot {
namespace fs = std::filesystem;
namespace {
[[noreturn]] void failure(const std::string &message) {
  throw BridgeError(message + ": " + std::strerror(errno));
}
bool beneath(const fs::path &path, const fs::path &root) {
  auto actual = path.begin();
  for (auto part = root.begin(); part != root.end(); ++part, ++actual)
    if (actual == path.end() || *part != *actual)
      return false;
  return true;
}
struct stat facts(int descriptor) {
  struct stat result {};
  if (::fstat(descriptor, &result))
    failure("Cannot inspect open file");
  return result;
}
std::array<std::int64_t, 4> timestamps(const struct stat &state) {
#ifdef __APPLE__
  return {state.st_mtimespec.tv_sec, state.st_mtimespec.tv_nsec,
          state.st_ctimespec.tv_sec, state.st_ctimespec.tv_nsec};
#else
  return {state.st_mtim.tv_sec, state.st_mtim.tv_nsec, state.st_ctim.tv_sec,
          state.st_ctim.tv_nsec};
#endif
}
} // namespace
FileStream::~FileStream() {
  if (value_ >= 0)
    ::close(value_);
}
FileStream::FileStream(FileStream &&other) noexcept : value_(other.value_) {
  other.value_ = -1;
}
FileStream &FileStream::operator=(FileStream &&other) noexcept {
  if (this != &other) {
    if (value_ >= 0)
      ::close(value_);
    value_ = other.value_;
    other.value_ = -1;
  }
  return *this;
}
std::vector<fs::path> PathGuard::defaults() {
  std::vector<fs::path> roots = {"/tmp", "/var/tmp", fs::temp_directory_path()};
  if (const auto *account = ::getpwuid(::getuid()))
    roots.emplace_back(account->pw_dir);
  else
    throw BridgeError("Cannot determine account home for file access");
  return roots;
}
PathGuard::PathGuard(std::vector<fs::path> roots) {
  if (roots.empty())
    throw BridgeError("At least one file-access root is required");
  for (const auto &root : roots) {
    const auto text = root.string();
    if (text.empty() || text.find('\0') != std::string::npos)
      throw BridgeError("Invalid file-access root");
    const auto canonical = fs::canonical(root);
    if (std::any_of(roots_.begin(), roots_.end(),
                    [&](const auto &entry) { return entry.path == canonical; }))
      continue;
    FileStream descriptor(::open(
        canonical.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (descriptor.get() < 0)
      failure("Cannot open file-access root");
    roots_.push_back({canonical, std::move(descriptor)});
  }
  std::sort(roots_.begin(), roots_.end(), [](const auto &a, const auto &b) {
    return a.path.string().size() > b.path.string().size();
  });
}
fs::path PathGuard::resolve(const std::string &input) const {
  if (input.empty() || input.size() > 4096 ||
      input.find('\0') != std::string::npos)
    throw BridgeError("File path is empty, too long or contains a null byte");
  auto current = fs::absolute(fs::path(input)).lexically_normal();
  std::vector<fs::path> missing;
  while (true) {
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory)
      throw BridgeError("Cannot inspect path: " + error.message());
    if (status.type() != fs::file_type::not_found && !error)
      break;
    if (current == current.root_path())
      throw BridgeError("No existing path ancestor");
    missing.push_back(current.filename());
    current = current.parent_path();
  }
  // canonical rejects dangling symlinks, including in an otherwise missing
  // path.
  auto resolved = fs::canonical(current);
  if (!missing.empty() && !fs::is_directory(resolved))
    throw BridgeError("Path parent is not a directory");
  for (auto part = missing.rbegin(); part != missing.rend(); ++part)
    resolved /= *part;
  for (const auto &root : roots_)
    if (beneath(resolved, root.path))
      return resolved;
  throw BridgeError("Path is outside allowed directories");
}
FileStream PathGuard::parent(const fs::path &path, bool create) const {
  const Root *root = nullptr;
  for (const auto &entry : roots_)
    if (beneath(path, entry.path)) {
      root = &entry;
      break;
    }
  if (!root || path == root->path)
    throw BridgeError("A file below an allowed directory is required");
  FileStream directory(::fcntl(root->descriptor.get(), F_DUPFD_CLOEXEC, 0));
  if (directory.get() < 0)
    failure("Cannot duplicate file-access root");
  for (const auto &part : path.parent_path().lexically_relative(root->path)) {
    if (part == "." || part.empty())
      continue;
    if (part == "..")
      throw BridgeError("Unexpected parent traversal");
    int opened = ::openat(directory.get(), part.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0 && errno == ENOENT && create) {
      if (::mkdirat(directory.get(), part.c_str(), 0700) && errno != EEXIST)
        failure("Cannot create screenshot directory");
      opened = ::openat(directory.get(), part.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (opened < 0)
      failure("Cannot open path parent without following a changed symlink");
    directory = FileStream(opened);
  }
  // Detect a root/ancestor replacement or rename since the descriptors were
  // opened.
  FileStream visible(::open(path.parent_path().c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (visible.get() < 0)
    failure("Cannot verify path parent");
  const auto anchored = facts(directory.get()), actual = facts(visible.get());
  if (anchored.st_dev != actual.st_dev || anchored.st_ino != actual.st_ino ||
      fs::canonical(path.parent_path()) != path.parent_path())
    throw BridgeError("Path parent changed during validation");
  return directory;
}
UploadEntry PathGuard::upload(const std::string &input,
                              bool allow_directory) const {
  const auto path = resolve(input);
  auto directory = parent(path, false);
  FileStream file(::openat(directory.get(), path.filename().c_str(),
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.get() < 0)
    failure("Cannot open upload file");
  const auto state = facts(file.get());
  const bool directory_input = S_ISDIR(state.st_mode);
  if (!S_ISREG(state.st_mode) && !(allow_directory && directory_input))
    throw BridgeError("Upload path must be a regular file");
  if (state.st_size < 0 ||
      static_cast<std::uint64_t>(state.st_size) > 1024ULL * 1024 * 1024)
    throw BridgeError("Upload file exceeds 1 GiB");
  return {path,
          static_cast<std::uint64_t>(state.st_dev),
          static_cast<std::uint64_t>(state.st_ino),
          directory_input ? 0 : static_cast<std::uint64_t>(state.st_size),
          timestamps(state),
          directory_input,
          std::move(file)};
}
void PathGuard::verify(const UploadEntry &file) const {
  const auto current = upload(file.path.string(), file.directory);
  if (current.path != file.path || current.device != file.device ||
      current.inode != file.inode || current.size != file.size ||
      current.times != file.times || current.directory != file.directory)
    throw BridgeError("Upload file changed during preparation");
}
std::vector<UploadEntry>
PathGuard::directory_files(const UploadEntry &folder) const {
  if (!folder.directory)
    throw BridgeError("Directory selection requires a directory");
  std::vector<UploadEntry> files;
  std::uint64_t total = 0;
  std::size_t visited = 0;
  for (auto iterator = fs::recursive_directory_iterator(folder.path);
       iterator != fs::recursive_directory_iterator(); ++iterator) {
    cancellation_point();
    if (++visited > 4096 || iterator.depth() > 32)
      throw BridgeError("Upload directory exceeds structural limits");
    const auto kind = iterator->symlink_status();
    if (fs::is_symlink(kind))
      throw BridgeError("Upload directory contains a symlink");
    if (fs::is_directory(kind))
      continue;
    files.push_back(upload(iterator->path().string()));
    total += files.back().size;
    if (files.size() > 128 || total > 1024ULL * 1024 * 1024)
      throw BridgeError("Upload directory exceeds 128 files or 1 GiB");
  }
  return files;
}
CaptureSink PathGuard::output(const std::string &input) const {
  const auto path = resolve(input);
  auto directory = parent(path, true);
  struct stat existing {};
  if (::fstatat(directory.get(), path.filename().c_str(), &existing,
                AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode))
      throw BridgeError("Screenshot destination must be a regular file");
  } else if (errno != ENOENT)
    failure("Cannot inspect screenshot destination");
  return CaptureSink(path, std::move(directory));
}
std::string CaptureSink::commit(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > maximum_image_bytes)
    throw BridgeError("Screenshot byte size is outside supported limits");
  FileStream visible(::open(path_.parent_path().c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (visible.get() < 0)
    failure("Screenshot parent is no longer accessible");
  const auto pinned = facts(parent_.get()), current = facts(visible.get());
  if (pinned.st_dev != current.st_dev || pinned.st_ino != current.st_ino ||
      fs::canonical(path_.parent_path()) != path_.parent_path())
    throw BridgeError("Screenshot parent changed before output");
  struct stat existing {};
  if (::fstatat(parent_.get(), path_.filename().c_str(), &existing,
                AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode))
      throw BridgeError("Screenshot destination changed to a non-file");
  } else if (errno != ENOENT)
    failure("Cannot verify screenshot destination");
  static std::atomic<std::uint64_t> sequence{0};
  std::string temporary;
  FileStream file;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    temporary = ".pagepilot-" + std::to_string(::getpid()) + "-" +
                std::to_string(sequence.fetch_add(1)) + ".tmp";
    const auto opened =
        ::openat(parent_.get(), temporary.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (opened >= 0) {
      file = FileStream(opened);
      break;
    }
    if (errno != EEXIST)
      failure("Cannot create screenshot temporary file");
  }
  if (file.get() < 0)
    throw BridgeError("Unable to reserve screenshot temporary file");
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      cancellation_point();
      const auto written =
          ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        failure("Cannot write complete screenshot");
      offset += static_cast<std::size_t>(written);
    }
    if (::fsync(file.get()))
      failure("Cannot flush screenshot");
    cancellation_point();
    if (::renameat(parent_.get(), temporary.c_str(), parent_.get(),
                   path_.filename().c_str()))
      failure("Cannot commit screenshot");
  } catch (...) {
    ::unlinkat(parent_.get(), temporary.c_str(), 0);
    throw;
  }
  return path_.string();
}
} // namespace pagepilot
