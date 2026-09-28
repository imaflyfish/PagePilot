#include <pagepilot/file_tools.hpp>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace pagepilot;
namespace fs = std::filesystem;
unsigned passed = 0, failed = 0;
void check(bool condition, const char *name) {
  if (condition)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << name << '\n';
  }
}
template <class F> void rejects(F action, const char *name) {
  try {
    action();
    check(false, name);
  } catch (const std::exception &) {
    check(true, name);
  }
}
void write(const fs::path &path, const std::string &value) {
  std::ofstream output(path, std::ios::binary);
  output << value;
  if (!output)
    throw BridgeError("Fixture write failed");
}
std::string bytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
int main() {
  auto pattern =
      (fs::temp_directory_path() / "pagepilot-paths-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    return 2;
  const auto base = fs::canonical(pattern), root = base / "allowed",
             outside = base / "outside";
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{base};
  try {
    fs::create_directory(root);
    fs::create_directory(outside);
    write(root / "input.txt", "owned");
    write(outside / "sentinel.txt", "untouched");
    PathGuard paths({root});
    check(paths.upload((root / "input.txt").string()).size == 5,
          "regular upload file inspected");
    check(
        paths.upload(
                 fs::relative(root / "input.txt", fs::current_path()).string())
                .path == root / "input.txt",
        "relative input canonicalized");
    rejects([&] { paths.upload(""); }, "empty path rejected");
    rejects([&] { paths.upload(std::string("a\0b", 3)); },
            "null byte rejected");
    rejects([&] { paths.upload((root / "missing").string()); },
            "missing upload rejected");
    write(root / "large.bin", "");
    if (::truncate((root / "large.bin").c_str(), 1024LL * 1024 * 1024 + 1))
      throw BridgeError("Unable to create sparse size-limit fixture");
    rejects([&] { paths.upload((root / "large.bin").string()); },
            "upload size limit checked without reading oversized file");
    fs::remove(root / "large.bin");
    rejects([&] { paths.upload(root.string()); },
            "directory is not a regular-file upload");
    rejects([&] { paths.upload((outside / "sentinel.txt").string()); },
            "outside upload refused");
    fs::create_directory(base / "allowed-sibling");
    write(base / "allowed-sibling/a", "no");
    rejects([&] { paths.upload((base / "allowed-sibling/a").string()); },
            "textual root prefix does not authorize sibling");
    rejects([&] { paths.output((root / "../outside/write.png").string()); },
            "parent traversal outside root refused");
    fs::create_symlink(root / "input.txt", root / "inside-link");
    check(paths.upload((root / "inside-link").string()).path ==
              root / "input.txt",
          "inside symlink resolved to canonical file");
    fs::create_symlink(outside / "sentinel.txt", root / "outside-link");
    rejects([&] { paths.upload((root / "outside-link").string()); },
            "outside symlink upload refused");
    rejects([&] { paths.output((root / "outside-link").string()); },
            "outside symlink output refused");
    fs::create_directory_symlink(outside, root / "directory-link");
    rejects([&] { paths.output((root / "directory-link/new/a.png").string()); },
            "missing output below outside symlink refused");
    fs::create_symlink(root / "absent", root / "dangling");
    rejects([&] { paths.output((root / "dangling").string()); },
            "dangling destination symlink refused");
    rejects([&] { paths.output((root / "dangling/new.png").string()); },
            "dangling ancestor symlink refused");
    rejects([&] { paths.output((root / "input.txt/a.png").string()); },
            "file cannot be output parent");
    ::mkfifo((root / "pipe").c_str(), 0600);
    rejects([&] { paths.upload((root / "pipe").string()); },
            "FIFO rejected without blocking open");
    rejects([&] { PathGuard invalid({outside / "sentinel.txt"}); },
            "file cannot be an allowed root");
    rejects([&] { PathGuard invalid(std::vector<fs::path>{}); },
            "empty allowed-root set rejected");
    const std::vector<std::uint8_t> image = {1, 2, 3, 4, 0, 255};
    auto target = paths.output((root / "new/sub/image.png").string());
    check(target.commit(image) == (root / "new/sub/image.png").string(),
          "output creates parents and returns canonical path");
    check(bytes(root / "new/sub/image.png") == std::string("\1\2\3\4\0\xff", 6),
          "output writes all binary bytes");
    write(root / "existing.png", "before");
    paths.output((root / "existing.png").string()).commit(image);
    check(bytes(root / "existing.png").size() == image.size(),
          "existing file replaced with complete new bytes");
    fs::create_symlink(root / "existing.png", root / "output-link");
    paths.output((root / "output-link").string()).commit(image);
    check(fs::is_symlink(root / "output-link") &&
              bytes(root / "existing.png").size() == image.size(),
          "inside output symlink preserves link and writes canonical target");
    bool remnants = false;
    for (const auto &entry : fs::directory_iterator(root))
      if (entry.path().filename().string().starts_with(".pagepilot-"))
        remnants = true;
    check(!remnants, "no temporary file remains after commit");
    auto replaced = paths.upload((root / "input.txt").string());
    write(root / "mutable.txt", "first");
    auto changed = paths.upload((root / "mutable.txt").string());
    fs::last_write_time(root / "mutable.txt",
                        fs::last_write_time(root / "mutable.txt") +
                            std::chrono::seconds(1));
    rejects([&] { paths.verify(changed); },
            "input modification metadata changed after preparation refused");
    fs::remove(root / "input.txt");
    fs::create_symlink(outside / "sentinel.txt", root / "input.txt");
    rejects([&] { paths.verify(replaced); },
            "input symlink substitution after preparation refused");
    auto pending = paths.output((root / "pending.png").string());
    fs::create_symlink(outside / "sentinel.txt", root / "pending.png");
    rejects([&] { pending.commit(image); },
            "destination symlink substitution refused");
    fs::create_directory(root / "parent");
    auto parent_swap = paths.output((root / "parent/image.png").string());
    fs::rename(root / "parent", root / "renamed");
    fs::create_directory_symlink(outside, root / "parent");
    rejects([&] { parent_swap.commit(image); },
            "renamed and replaced output parent refused");
    check(bytes(outside / "sentinel.txt") == "untouched" &&
              !fs::exists(outside / "image.png"),
          "path refusals leave outside content untouched");
    fs::create_directory(root / "pinned");
    PathGuard pinned({root / "pinned"});
    fs::rename(root / "pinned", root / "prior-root");
    fs::create_directory(root / "pinned");
    rejects([&] { pinned.output((root / "pinned/image.png").string()); },
            "allowed root replacement does not redirect pinned descriptor");
    const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADE"
                            "lEQVR4nGNgaPgPAAIDAYAkYfWXAAAAAElFTkSuQmCC";
    const auto decoded = decode_png(png);
    check(decoded.width == 1 && decoded.height == 1 &&
              decoded.bytes.size() == 69,
          "complete PNG base64 decoded and checksummed");
    rejects([&] { decode_png(png.substr(0, png.size() - 4)); },
            "truncated PNG ending refused");
    auto damaged = png;
    damaged[50] = damaged[50] == 'A' ? 'B' : 'A';
    rejects([&] { decode_png(damaged); },
            "PNG corruption rejected by chunk checksum");
    rejects([&] { decode_png("////"); }, "non-PNG payload rejected");
    rejects([&] { decode_png("=AAA"); }, "leading base64 padding rejected");
    rejects([&] { decode_png("AA=A"); }, "nonterminal base64 padding rejected");
    rejects([&] { decode_png("AB=="); }, "noncanonical pad bits rejected");
    rejects([&] { decode_png("AAAA\n"); },
            "base64 framing whitespace rejected");
    rejects([&] { decode_png(std::string(28 * 1024 * 1024, 'A')); },
            "oversized screenshot rejected before allocation");
    std::cout << passed << " file contract checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
