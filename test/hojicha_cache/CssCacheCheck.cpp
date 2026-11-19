#include <CssParser.h>

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

std::string cacheBytes() {
  std::ifstream input("css_rules.cache", std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
  CssParser parser(".");
  assert(parser.beginCacheAppend());
  assert(parser.endCacheAppend(false));
  const std::string original = cacheBytes();
  assert(!original.empty());
  assert(parser.validateCache());
  assert(cacheBytes() == original);
  {
    std::ofstream output("css_rules.cache", std::ios::binary | std::ios::trunc);
    output.write(original.data(), 1);
  }
  assert(!parser.validateCache());
  {
    std::ofstream output("css_rules.cache", std::ios::binary | std::ios::trunc);
    const char invalidVersion = 0;
    output.write(&invalidVersion, 1);
  }
  assert(!parser.validateCache());
  assert(!parser.hasCache());
  assert(parser.beginCacheAppend());
  assert(parser.endCacheAppend(false));
  assert(parser.validateCache());
}
