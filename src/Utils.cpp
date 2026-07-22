#include "Utils.h"

#include <algorithm>
#include <cctype>

int Utils::indexOf(const std::vector<std::string>& list, const std::string& v) {
    auto it = std::find(list.begin(), list.end(), v);
    if (it == list.end()) return -1;
    return static_cast<int>(std::distance(list.begin(), it));
}

std::string Utils::toUpperCase(const char* str) {
    if (str == nullptr) {
        return std::string();
    }

    // Returning std::string (rather than a caller-owned `new char[]`, as
    // this used to) avoids the memory leak every single call site had: none
    // of them ever freed the buffer this returned.
    std::string upper(str);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}
