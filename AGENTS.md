# Repository Guidelines

Detailed repository rules live in local Codex skills under `.codex/skills/`.

Reminder:
- Use `kv-general` for any code changes.
- Use `kv-cpp` for C++ code changes.
- Use `kv-workflows` for configure/build/run/test tasks, especially `devenv tasks run proj:*`.
- Use `kv-contracts` for commits/PRs.

# General rules
- Never implement backwards compatibility or silent fallbacks unless told to do so.
- Prefer failing hard over silent fallbacks.
- Never introduce environment variables unless told to do so.

# Response discipline
- Do not respond with large blocks of code; show only short, focused snippets when necessary, or omit code entirely and describe changes instead.
- Verify, don't recall: reread active code/logs; test exact endpoints.
- Prefer authoritative data: no guessing or fallback to stale/synthetic for critical logic.
- Always say what the contents of replies are based on (memory, repo code, docs, tool output).
