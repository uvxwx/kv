---
name: kv-workflows
description: Build, run, and test workflow
---

# Workflows

Use this when the task involves building, running, or testing.

## Toolchain context
- C++20 is required to build; forced by upstream dependencies.
- The primary toolchain comes from `nix/toolchain.nix` and uses `pkgs.llvmPackages_21`.
- userver is consumed via the Nix flake in `nix/userver`.
- CMake configures `./kv` with Ninja into `build/kv/{san,release,...}`.

## Agent sandbox limits
- If a command fails due to permissions, rerun it with escalation rather than inventing an alternate workflow.

## Preferred tasks
- Build the default sanitizer/dev variant with `devenv tasks run proj:devBuild`.
- Run the test flow with `devenv tasks run proj:devTest`.

## Runtime and test details
- The dev shell exports `USERVER_DIR`, `USERVER_PYTHON`, `PYTHONPATH`, and helper commands such as `test_san`.

## Binary invocation
- When passing config vars on the CLI, use `--config_vars` with underscore.
- The binary and runtime scripts also use `--config_vars_override` with underscores.
