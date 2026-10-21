// Fel: felkod och undantag som alla bibliotek kastar.
#pragma once

#include <stdexcept>
#include <string>

namespace pc {

enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument = 1,
    FileNotFound = 2,
    FileFormat = 3,
    PdfError = 4,
    Cancelled = 5,
    NotFound = 6,
    Internal = 99,
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& msg) : std::runtime_error(msg), code_(code) {}
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

}  // namespace pc
