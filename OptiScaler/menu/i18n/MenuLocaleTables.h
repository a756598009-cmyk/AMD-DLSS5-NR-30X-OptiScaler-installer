#pragma once

#include <cstddef>

namespace MenuLocale
{
// Whether a translation keeps the English text's printf conversions and {} in order, without a stray "% d".
bool TranslationIsSafe(const char* english, const char* translation);

namespace Tables
{
struct Entry
{
    const char* key;
    const char* text;
};

size_t KeyCount();
const char* Key(size_t index);

bool Has(const char* code);
const Entry* Find(const char* code, size_t& count);
} // namespace Tables
} // namespace MenuLocale
