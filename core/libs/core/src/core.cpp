#include "printcmp/core/core.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>

namespace pc {

Mat3 Mat3::inverse() const {
    const double* a = m;
    double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                 a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (std::abs(det) < 1e-300) throw Error(ErrorCode::Internal, "singulär matris");
    Mat3 r;
    double* o = r.m;
    o[0] = (a[4] * a[8] - a[5] * a[7]) / det;
    o[1] = (a[2] * a[7] - a[1] * a[8]) / det;
    o[2] = (a[1] * a[5] - a[2] * a[4]) / det;
    o[3] = (a[5] * a[6] - a[3] * a[8]) / det;
    o[4] = (a[0] * a[8] - a[2] * a[6]) / det;
    o[5] = (a[2] * a[3] - a[0] * a[5]) / det;
    o[6] = (a[3] * a[7] - a[4] * a[6]) / det;
    o[7] = (a[1] * a[6] - a[0] * a[7]) / det;
    o[8] = (a[0] * a[4] - a[1] * a[3]) / det;
    return r;
}

const char* stage_name(Stage s) {
    static const char* names[] = {"render", "locate", "register", "normalize", "diff", "folds",
                                  "classify", "aggregate", "text", "data_check", "report", "done"};
    return names[static_cast<int>(s)];
}

double median(std::vector<double> v) {
    if (v.empty()) return std::nan("");
    size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    double hi = v[n / 2];
    if (n % 2) return hi;
    double lo = *std::max_element(v.begin(), v.begin() + n / 2);
    return (lo + hi) / 2;
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    double pos = q / 100.0 * (v.size() - 1);
    size_t i = static_cast<size_t>(std::floor(pos));
    size_t j = std::min(i + 1, v.size() - 1);
    double f = pos - i;
    return v[i] + (v[j] - v[i]) * f;
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return std::nan("");
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

std::string base64(const std::vector<unsigned char>& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = data[i] << 16;
        if (i + 1 < data.size()) v |= data[i + 1] << 8;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += (i + 1 < data.size()) ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::vector<unsigned char> read_file_bytes(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw Error(ErrorCode::FileNotFound, "kan inte öppna " + path);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string read_file(const std::string& path) {
    auto b = read_file_bytes(path);
    return std::string(b.begin(), b.end());
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw Error(ErrorCode::FileNotFound, "kan inte skriva " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#x27;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < static_cast<int>(sizeof buf)) return std::string(buf, std::max(0, n));
    std::string big(n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize(n);
    return big;
}

}  // namespace pc
