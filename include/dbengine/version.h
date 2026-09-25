#pragma once

#include <string_view>

#define DBENGINE_VERSION_MAJOR 0
#define DBENGINE_VERSION_MINOR 1
#define DBENGINE_VERSION_PATCH 0

namespace dbengine {

inline constexpr std::string_view kVersion = "0.1.0";
inline constexpr int kVersionMajor = DBENGINE_VERSION_MAJOR;
inline constexpr int kVersionMinor = DBENGINE_VERSION_MINOR;
inline constexpr int kVersionPatch = DBENGINE_VERSION_PATCH;

}  // namespace dbengine
