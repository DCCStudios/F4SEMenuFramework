#include "Utils.h"

int Utils::indexOf(const std::vector<std::string>& list, const std::string& v) {
    auto it = std::find(list.begin(), list.end(), v);
    if (it == list.end()) return 0;
    return static_cast<int>(std::distance(list.begin(), it));
}
char* Utils::toUpperCase(const char* str) {
    if (str == nullptr) {
        return nullptr;
    }

    char* upper_str = new char[strlen(str) + 1];
    if (upper_str == nullptr) {
        return nullptr;
    }
    for (int i = 0; str[i] != '\0'; i++) {
        upper_str[i] = std::toupper((unsigned char)str[i]);
    }
    upper_str[strlen(str)] = '\0';

    return upper_str;
}