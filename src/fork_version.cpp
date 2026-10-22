#include "version.h"

// Bumped with each v1.x tag of this fork.  VersionInfo::version stays as
// upstream ships it, so it names the upstream release this one is based on.
const std::string VersionInfo::fork_version = "v1.7/by larry";

std::string VersionInfo::full() {
  return fork_version + " (upstream " + version + ")";
}
