#include "input_ack.hpp"
#include <algorithm>
#include <array>
#include <pagepilot/file_tools.hpp>
#include <cmath>
#include <limits>
namespace pagepilot {
JsonDoc DomTools::capture_bounds(const std::string &selector) {
  BrowserSession::PageScope page(browser_);
  auto element = locate({{"selector", selector}}, true);
  browser_.reveal_frames(clock_.remaining());
  element.call({{"operation", "scroll"}}, clock_.remaining());
  browser_.settle_layout(clock_.remaining());
  const auto box = element.call(
      {{"operation", "read"}, {"type", "bounding_box"}}, clock_.remaining());
  if (!box.contains("width"))
    throw BridgeError("Screenshot element has no visible bounds");
  const double x = box.at("x"), y = box.at("y"), width = box.at("width"),
               height = box.at("height");
  double left = std::numeric_limits<double>::infinity(), top = left,
         right = -left, bottom = -left;
  for (const auto &[dx, dy] : std::array<std::pair<double, double>, 4>{
           {{0, 0}, {width, 0}, {width, height}, {0, height}}}) {
    const auto point = browser_.project_point({{"x", x + dx}, {"y", y + dy}},
                                              clock_.remaining(), false);
    left = std::min(left, point.at("x").get<double>());
    right = std::max(right, point.at("x").get<double>());
    top = std::min(top, point.at("y").get<double>());
    bottom = std::max(bottom, point.at("y").get<double>());
  }
  return {{"x", left},
          {"y", top},
          {"width", right - left},
          {"height", bottom - top}};
}
JsonDoc FileTools::upload(const JsonDoc &arguments) {
  BrowserSession::PageScope page(browser_);
  const auto names = arguments.at("files").is_array()
                         ? arguments.at("files")
                         : JsonDoc::array({arguments.at("files")});
  if (names.size() > 128)
    throw BridgeError("Upload batch exceeds 128 files");
  std::vector<UploadEntry> files, members;
  JsonDoc paths = JsonDoc::array();
  std::uint64_t total = 0;
  bool directory = false;
  for (const auto &name : names) {
    files.push_back(paths_.upload(name.get<std::string>(), true));
    if (files.back().directory) {
      if (names.size() != 1)
        throw BridgeError("A directory upload must be the only selected path");
      directory = true;
      members = paths_.directory_files(files.back());
    }
    total += files.back().size;
    if (total > 1024ULL * 1024 * 1024)
      throw BridgeError("Upload batch exceeds 1 GiB");
    paths.push_back(files.back().path.string());
  }
  DomTools dom(browser_, clock_.remaining());
  auto input = dom.locate({{"selector", arguments.at("selector")}});
  const auto kind =
      input.call({{"operation", "file_input"}}, clock_.remaining());
  if (names.size() > 1 && kind.at("multiple") != true)
    throw BridgeError("File input does not accept multiple files");
  if (!names.empty() && kind.at("directory").get<bool>() != directory)
    throw BridgeError("Directory paths require a directory file input; regular "
                     "files require a normal file input");
  for (const auto &file : files)
    paths_.verify(file);
  for (const auto &file : members)
    paths_.verify(file);
  if (names.empty()) {
    input.call({{"operation", "clear_files"}}, clock_.remaining());
    // Clearing is a synchronous DOM operation with synthetic events. It does
    // not produce the trusted native-selection receipt used below.
    browser_.evaluate("new Promise(resolve=>setTimeout(()=>resolve(true),0))",
                      clock_.remaining());
  } else {
    // Acknowledgement of setFileInputFiles does not imply that asynchronous
    // directory enumeration has finished. Bind completion to this selection's
    // trusted event, not a count which a prior selection may already satisfy.
    InputAck receipt(browser_, input.identity(), "file-selection", 0,
                         clock_.remaining());
    browser_.session_call(input.session(), "DOM.setFileInputFiles",
                          {{"objectId", input.identity()}, {"files", paths}},
                          clock_.remaining());
    receipt.finish(clock_.remaining());
  }
  const auto after =
      input.call({{"operation", "file_input"}}, clock_.remaining());
  if (after.at("count") != (directory ? members.size() : names.size()))
    throw BridgeError(
        "Browser file selection did not match the requested count");
  return {{"uploaded", names.size()}};
}
JsonDoc FileTools::screenshot(const JsonDoc &arguments) {
  BrowserSession::PageScope page(browser_);
  std::optional<CaptureSink> destination;
  if (arguments.contains("path"))
    destination.emplace(paths_.output(arguments.at("path").get<std::string>()));
  browser_.current_session();
  JsonDoc clip;
  const bool selected = arguments.contains("selector");
  if (selected) {
    DomTools dom(browser_, clock_.remaining());
    clip = dom.capture_bounds(arguments.at("selector"));
  }
  browser_.settle_layout(clock_.remaining());
  const auto layout = browser_.page_call("Page.getLayoutMetrics",
                                         JsonDoc::object(), clock_.remaining());
  const auto viewport = layout.at("cssVisualViewport");
  if (selected) {
    clip["x"] = clip.at("x").get<double>() + viewport.at("pageX").get<double>();
    clip["y"] = clip.at("y").get<double>() + viewport.at("pageY").get<double>();
  } else if (arguments.value("fullPage", false))
    clip = layout.at("cssContentSize");
  else
    clip = {{"x", viewport.at("pageX")},
            {"y", viewport.at("pageY")},
            {"width", viewport.at("clientWidth")},
            {"height", viewport.at("clientHeight")}};
  double x = clip.at("x"), y = clip.at("y"), width = clip.at("width"),
         height = clip.at("height");
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
      !std::isfinite(height) || width <= 0 || height <= 0)
    throw BridgeError("Invalid screenshot bounds");
  const auto right = std::ceil(x + width), bottom = std::ceil(y + height);
  x = std::max(0.0, std::floor(x));
  y = std::max(0.0, std::floor(y));
  width = right - x;
  height = bottom - y;
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
      width * height > 32000000)
    throw BridgeError(
        "Screenshot bounds exceed 32768 per edge or 32 million CSS pixels");
  const auto density = browser_
                           .page_call("Runtime.evaluate",
                                      {{"expression", "devicePixelRatio"},
                                       {"returnByValue", true}},
                                      clock_.remaining())
                           .at("result")
                           .at("value")
                           .get<double>();
  if (!std::isfinite(density) || density <= 0 || width * density > 32768 ||
      height * density > 32768 || width * height * density * density > 32000000)
    throw BridgeError("Screenshot physical dimensions exceed 32768 per edge or "
                     "32 million pixels");
  clip = {
      {"x", x}, {"y", y}, {"width", width}, {"height", height}, {"scale", 1}};
  const auto captured =
      browser_.page_call("Page.captureScreenshot",
                         {{"format", "png"},
                          {"fromSurface", true},
                          {"captureBeyondViewport",
                           selected || arguments.value("fullPage", false)},
                          {"clip", clip}},
                         clock_.remaining());
  const auto &encoded = captured.at("data").get_ref<const std::string &>();
  const auto image = decode_png(encoded);
  JsonDoc result = {{"size", image.bytes.size()},
                 {"width", image.width},
                 {"height", image.height},
                 {"mimeType", "image/png"}};
  if (destination)
    result["saved"] = destination->commit(image.bytes);
  else
    result["screenshot"] = encoded;
  return result;
}
} // namespace pagepilot
