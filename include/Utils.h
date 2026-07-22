class Utils {
public:
    // Standard indexOf semantics: returns -1 (not 0) when `v` isn't found in
    // `list`. Callers that use the result as a vector index must guard
    // against -1 themselves (see Config::Init / UI::RenderConfigWindow).
    static int indexOf(const std::vector<std::string>& list, const std::string& v);
    static std::string toUpperCase(const char* str);
};