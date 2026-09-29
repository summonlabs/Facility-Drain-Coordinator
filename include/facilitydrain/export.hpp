// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_EXPORT_HPP
#define FACILITYDRAIN_EXPORT_HPP

// The library is built as a static archive by default. When built shared, the
// public surface is explicitly exported and everything else stays hidden so the
// installed package never leaks internal symbols.
#if defined(_WIN32) && defined(FACILITYDRAIN_SHARED)
#if defined(FACILITYDRAIN_BUILDING_LIBRARY)
#define FACILITYDRAIN_API __declspec(dllexport)
#else
#define FACILITYDRAIN_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(FACILITYDRAIN_SHARED)
#define FACILITYDRAIN_API __attribute__((visibility("default")))
#else
#define FACILITYDRAIN_API
#endif

#if !defined(__cplusplus) || __cplusplus < 202002L
#if !defined(_MSVC_LANG) || _MSVC_LANG < 202002L
#error "Facility Drain Coordinator requires C++20 or later."
#endif
#endif

#if defined(_MSC_VER)
// /Zc:__cplusplus is enabled by the build system, so __cplusplus is honest here.
#define FACILITYDRAIN_COMPILER_MSVC 1
#elif defined(__clang__)
#define FACILITYDRAIN_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define FACILITYDRAIN_COMPILER_GCC 1
#endif

#endif // FACILITYDRAIN_EXPORT_HPP
