// Filer, kodning och formatering.
#pragma once

#include <string>
#include <vector>

namespace pc {

std::string base64(const std::vector<unsigned char>& data);
std::string read_file(const std::string& path);
std::vector<unsigned char> read_file_bytes(const std::string& path);
void write_file(const std::string& path, const std::string& data);
std::string html_escape(const std::string& s);
std::string sha256_hex(const unsigned char* data, size_t len);
std::string sha256_hex(const std::string& s);
std::string format(const char* fmt, ...);

}  // namespace pc
