#include <iostream>

#include "dbengine/version.h"

int main() {
  std::cout << "dbengine version " << dbengine::kVersion << '\n';
  return 0;
}
