#include <cassert>
#include <string>

#include "dbengine/version.h"

int main() {
  // Smoke: always true
  assert(true);

  // Version check: header constants must match version string
  assert(dbengine::kVersionMajor == 0);
  assert(dbengine::kVersionMinor == 1);
  assert(dbengine::kVersionPatch == 0);
  assert(std::string(dbengine::kVersion) == "0.1.0");

  return 0;
}
