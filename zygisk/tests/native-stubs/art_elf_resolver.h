#pragma once

#include <string>
#include <string_view>

// Hidden-symbol resolution is outside this test's cold exported-symbol fixture.
class ArtElfResolver {
  public:
    bool open_loaded(std::string_view, std::string *) { return false; }
    void *find(std::string_view) const { return nullptr; }
};
