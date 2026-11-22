#pragma once
// MSVC has no GCC attributes; upstream lmxxf marks a few functions with __attribute__((noinline)).
#define __attribute__(x)
