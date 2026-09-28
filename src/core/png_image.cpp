#include <algorithm>
#include <array>
#include <boost/crc.hpp>
#include <pagepilot/file_tools.hpp>
namespace pagepilot {
namespace {
std::uint32_t word(const std::vector<std::uint8_t> &data, std::size_t offset) {
  return (std::uint32_t(data.at(offset)) << 24) |
         (std::uint32_t(data.at(offset + 1)) << 16) |
         (std::uint32_t(data.at(offset + 2)) << 8) |
         std::uint32_t(data.at(offset + 3));
}
} // namespace
PngImage decode_png(const std::string &text) {
  constexpr std::size_t maximum = 20 * 1024 * 1024;
  if (text.empty() || text.size() % 4 || text.size() > 4 * ((maximum + 2) / 3))
    throw BridgeError(
        "Screenshot Base64 size is invalid or exceeds 20 MiB decoded");
  std::array<int, 256> alphabet;
  alphabet.fill(-1);
  const std::string digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t i = 0; i < digits.size(); ++i)
    alphabet[static_cast<unsigned char>(digits[i])] = static_cast<int>(i);
  std::vector<std::uint8_t> data;
  data.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    if (i % 16384 == 0)
      cancellation_point();
    int values[4]{};
    unsigned padding = 0;
    for (unsigned k = 0; k < 4; ++k) {
      const auto character = static_cast<unsigned char>(text[i + k]);
      if (character == '=') {
        if (k < 2)
          throw BridgeError("Invalid Base64 padding");
        ++padding;
      } else {
        if (padding || alphabet[character] < 0)
          throw BridgeError("Invalid Base64 character");
        values[k] = alphabet[character];
      }
    }
    if (padding &&
        (i + 4 != text.size() || (padding == 2 && (values[1] & 15)) ||
         (padding == 1 && (values[2] & 3))))
      throw BridgeError("Noncanonical Base64 padding");
    data.push_back(
        static_cast<std::uint8_t>((values[0] << 2) | (values[1] >> 4)));
    if (padding < 2)
      data.push_back(
          static_cast<std::uint8_t>((values[1] << 4) | (values[2] >> 2)));
    if (!padding)
      data.push_back(static_cast<std::uint8_t>((values[2] << 6) | values[3]));
  }
  const std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  if (data.size() > maximum || data.size() < 33 ||
      !std::equal(signature.begin(), signature.end(), data.begin()))
    throw BridgeError("Screenshot is not a bounded PNG");
  bool header = false, pixels = false, ended = false;
  unsigned width = 0, height = 0;
  for (std::size_t offset = 8; offset < data.size();) {
    cancellation_point();
    if (data.size() - offset < 12)
      throw BridgeError("Truncated PNG chunk");
    const auto length = static_cast<std::size_t>(word(data, offset));
    if (length > data.size() - offset - 12)
      throw BridgeError("Truncated PNG chunk payload");
    const std::string kind(
        reinterpret_cast<const char *>(data.data() + offset + 4), 4);
    boost::crc_32_type checksum;
    checksum.process_bytes(data.data() + offset + 4, length + 4);
    if (checksum.checksum() != word(data, offset + 8 + length))
      throw BridgeError("PNG chunk checksum mismatch");
    if (!header) {
      if (kind != "IHDR" || length != 13)
        throw BridgeError("PNG lacks initial image header");
      width = word(data, offset + 8);
      height = word(data, offset + 12);
      header = true;
      if (!width || !height || width > 32768 || height > 32768 ||
          std::uint64_t(width) * height > 32000000)
        throw BridgeError(
            "Screenshot dimensions exceed 32768 per edge or 32 million pixels");
    } else if (kind == "IHDR")
      throw BridgeError("Repeated PNG header");
    if (kind == "IDAT")
      pixels = true;
    if (kind == "IEND") {
      if (length || !pixels || offset + 12 != data.size())
        throw BridgeError("Invalid PNG ending");
      ended = true;
    }
    offset += length + 12;
  }
  if (!ended)
    throw BridgeError("PNG is missing its ending");
  return {std::move(data), width, height};
}
} // namespace pagepilot
