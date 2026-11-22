// Built with /utf-8: the tables are UTF-8 and the menu draws UTF-8 whatever the build machine's code page.
#include "MenuLocaleTables.h"

#include <cstring>
#include <iterator>

namespace MenuLocale::Tables
{
namespace
{
const char* const kKeys[] = {
#include "keys.inl"
};

struct Language
{
    const char* code;
    const Entry* entries;
    size_t count;
};

// clang-format off
#include "languages.inl"
// clang-format on
} // namespace

size_t KeyCount() { return std::size(kKeys); }
const char* Key(size_t index) { return kKeys[index]; }

const Entry* Find(const char* code, size_t& count)
{
    for (const Language& language : kLanguages)
    {
        if (strcmp(code, language.code) == 0)
        {
            count = language.count;
            return language.entries;
        }
    }

    count = 0;
    return nullptr;
}

bool Has(const char* code)
{
    size_t count = 0;
    return Find(code, count) != nullptr;
}
} // namespace MenuLocale::Tables
