#include <cstdio>

#include "primeparts/config.h"

int main() {
  const std::string text = primeparts::config::RenderExample();
  std::fwrite(text.data(), 1, text.size(), stdout);
  return 0;
}
