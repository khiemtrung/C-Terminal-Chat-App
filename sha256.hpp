#pragma once

#include <string>

// Returns the lowercase hexadecimal SHA-256 digest of `input`.
// Implemented in plain C++ so the project has no crypto dependencies.
std::string sha256_hex(const std::string& input);
