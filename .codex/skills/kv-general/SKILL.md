---
name: kv-general
description: General repository rules. Use whenever making code changes in this repository.
---

# General

Use these rules whenever making code changes in this repository.

## Project overview
- The project implements a transactional concurrent key-value store.
- Tooling configs at repo root define formatting and checks.

## Project structure
- `src/` is the only allowed location for `.cpp` sources.
- Shared handlers/components headers MUST live in `include/`.
- C++ unit tests and pytest tests MUST live under `test/`.
