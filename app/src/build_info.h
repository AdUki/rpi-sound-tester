#pragma once

namespace st {

// Which build this is, for GET /api/state `version`. Defined in build_info.cpp, which CMake writes
// into the build tree on every build (cmake/build_info.cmake) and links into soundtesterd only, so
// nothing else rebuilds for it.
extern const char* const kBuildGit;   // `git describe --always --dirty`, or "unknown"
extern const char* const kBuildTime;  // UTC, ISO 8601; SOURCE_DATE_EPOCH when that is set

}  // namespace st
