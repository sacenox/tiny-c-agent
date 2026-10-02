# Project file for agents

## c-agent (`cage` for short)

A fast, minimal and config first agent for your terminal.

## References

- `../mini-coder` - A very similar implementation in typescript. An example do not blindly copy.

## Some guardrails

- **Write original code.** Other coding agents are behavioral references only.
- **No loop limits.** No max turns, tool calls, token budgets, or agent-imposed
  timeouts. The user's ability to interrupt is the limit.
- **Config-first.** Every user-facing behavior that can vary comes from config
  with a sane default.
- **Minimal context.** The model sees the configured system prompt plus
  explicitly opted-in resource files — never reminders, hidden blocks, or
  harness meta-text.
- **The agent never imports the TUI.** Headless and TUI are two projections of
  the same agent events; the TUI owns no agent or provider semantics.
- **Respect linux/unix coventions**. Both for storing configs and state, as well as making sure the head
  less mode is scriptable.

## Invariants

- Sessions are append-only JSONL, one message per line, fsynced on
  write. Never rewrite or delete committed lines. A persistence failure stops
  the turn.
- The loop has no compaction, no retries, and no meta-messages between steps.
  Provider errors surface as-is, never masked or summarized.
- Single codemode-style LLM tool, exectutes the script in a JS environment.
- Interruption: ESC politely pauses at the next step boundary; Ctrl+C cancels the
  turn (abort the request, kill the tool's process group, persist the aborted
  message); Ctrl+C (or CTRL+D) exits on an empty draft. Completed side effects are never
  undone.
- Diagnostics are local only. No telemetry, no hidden stalls.

## Working agreement

- Trace the real data flow before designing; apply guards at the narrowest
  boundary.
- Keep the diff small. Do not opportunistically refactor or harden adjacent code.
- Do not add tests unless explicitly asked; verify manually.
