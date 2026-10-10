#pragma once
#include <string>

struct VersionInfo {
  static const std::string version;
  static const std::string copyright;

  // This fork's release, and how it reads beside the upstream one it is
  // based on; both live in fork_version.cpp, away from upstream's bumps.
  static const std::string fork_version;
  static std::string full();
};
