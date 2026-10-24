#pragma once

/*
 * Export/import macros for the crossserial library.
 *
 *   - building the shared library  -> CROSSSERIAL_EXPORTS defined by CMake
 *   - consuming the shared library -> nothing (dllexport/dllimport picked below)
 *   - static build / consumption   -> CROSSSERIAL_STATIC_DEFINE
 */

#if defined(_WIN32) || defined(_WIN64)
#  if defined(CROSSSERIAL_EXPORTS)
#    define CS_API __declspec(dllexport)
#  elif defined(CROSSSERIAL_STATIC_DEFINE)
#    define CS_API
#  else
#    define CS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define CS_API __attribute__((visibility("default")))
#else
#  define CS_API
#endif

