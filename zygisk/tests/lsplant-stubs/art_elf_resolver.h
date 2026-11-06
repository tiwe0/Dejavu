#pragma once
#include <string>
#include <string_view>
class ArtElfResolver {
  public:
    bool open_loaded(std::string_view, std::string *) {
        return false;
    }
    void *find(std::string_view) const {
        return nullptr;
    }
    void *find_prefix(std::string_view) const {
        return nullptr;
    }
};
