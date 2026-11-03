// Host check: clang++ -std=c++17 -Iinclude tests/jiecang_model_test.cpp src/jiecang_rj12.cpp -o jiecang_model_test.exe
#include <cassert>
#include <cstring>
#include <initializer_list>

#include "pandadesk/jiecang_rj12.hpp"

int main() {
  using namespace pandadesk::jiecang_rj12;
  assert(std::strcmp(normalize_model("JIECANG_GENERIC"), "JIECANG_GENERIC") == 0);
  assert(std::strcmp(normalize_model("JCHT35K72C"), "JCHT35K72C") == 0);
  for (const char *legacy : {"JCB35M11C", "MDS36DR", "JCB36N2CA", "JCB36N2CA-230",
                             "JCB36N2HAG-230", "JCHT35K9-003-v4", "JCB36NE2", "JCB36M",
                             "JCB36NE2A-230"}) {
    assert(std::strcmp(normalize_model(legacy), "JIECANG_GENERIC") == 0);
  }
  for (const char *invalid : {"", "unknown", "JCHT35K72C-unknown", "FullyCB2C-A", "JCB35N2"}) {
    assert(normalize_model(invalid) == nullptr);
  }
  assert(normalize_model(nullptr) == nullptr);
}
