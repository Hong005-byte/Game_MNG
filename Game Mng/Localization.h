#pragma once
#include <string>

// Minimal two-language string table. Covers the graphical world view and the
// top-level startup prompts; the deep console menu text (achievements, event
// flavor, per-action messages) is intentionally not covered here yet — that's
// a much larger, separate translation pass across many files.
enum class Language { English, Chinese };

class Localization {
public:
    static Language current;

    // Looks up `key`; if missing, returns `key` itself so a typo never crashes,
    // it just shows up untranslated (easy to spot).
    static const std::string& t(const std::string& key);
};
