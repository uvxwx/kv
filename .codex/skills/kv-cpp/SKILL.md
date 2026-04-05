---
name: kv-cpp
description: C++ coding rules for the. Use when changing C++ code.
---

# C++

Use these rules whenever making C++ changes in this repository.

## Language and headers
- We use C++26
- All shared headers under `include/` MUST use `#pragma once`.

## Style and naming
- Classes MUST use PascalCase (for example, `ByPrefixHandler`).
- Functions and variables MUST use lowerCamelCase.
- Constants MUST use the `kName` form.
- Default parameters in function declarations or definitions are forbidden.
- Namespace rules MUST be strictly followed: reuse the existing `namespace us = userver;` pattern where applicable, and use `::name` for global symbols.
- Use `{}` instead of `std::nullopt` in return statements and obvious initialization sites whenever it compiles.
- Use `size_t`, `int64_t` (not `std::size_t` or `std::int64_t`).
- Never use `Type name = Type(...)`; use `Type name{...}` instead to avoid writing the type twice.
- Filenames MUST be snake_case (for example, `ip_utils.cpp`).
- Declarations and definitions MUST exactly match (names and signatures).
- Do not introduce duplicate code; factor common logic into reusable helpers.
- Prefer `std::begin`/`std::end` over calling `.begin()`/`.end()` on containers when passing iterators.
- Postfix arithmetic (`++`, `--`) MUST be used by default.
- Class members must not use a trailing underscore naming style; use regular lowerCamelCase for member variables.
- Never call `std::chrono::system_clock::now()`; use `userver::utils::datetime::Now()` instead.
- Mutable lambdas are forbidden.
- Catch-all exception handlers are forbidden.
- Never use `return ReturnType(...)`; when constructing a value to return, prefer `return {...};` wherever it compiles.
- Never use `std::*stream*`, syscalls, or C's stdlib for I/O; use either `fmt` or userver I/O functionality only.
- Never use `static_cast<IntType>`.

## [[nodiscard]] usage
- Do not annotate destructors, move operations, or obvious mutators.
- Favor annotating; compilers will surface accidental value drops.
- The `[[nodiscard]]` rules in this section are mandatory and MUST be followed strictly.

## Testing expectations
- C++ tests use `userver::utest` and are wired from `test/CMakeLists.txt`.
