# c-agent (`cage` for short)

A fast, minimal and config-first agent for your terminal.

## Config

The config lives at the user's configured config folder (usually ~/.config).

A custom config can be provided via `--config/-c [path to file]` as a CLI argument.
If a custom config was specified, and there is a global config, they are merged with the local
config overriding the global.

Config schema:

```jsonc
{
    "theme": "tokyo-night", // string | undefined, the color palette to use.
    "provider": "", // string | undefined
    "model": "", // string | undefined
    "effort": "", // string| undefined; off, minimal, low, medium, high, xhigh, max, depends on model, needs to be clamped on model selection. Default is we don't send one, let the provider decide.
    "systemPrompt": "", // path | undefined, the path to a markdown or text file to use as system prompt
    "discoverAgentFiles": true, // boolean | true, if we should discover and append AGENTS.md to the prompt
    "skillsDirs": [], // [path, ...] | [], list of directories to search for SKILL.md files.
    "sessionsDir": "", // path | undefined, defaults to .local/state or the appropriate Unix convention,
    "customProviders": [], // see right below.
}
```

Custom providers example:

```json
{
  "customProviders": [
    {
      "id": "ollama",
      "name": "Ollama",
      "baseUrl": "http://localhost:11434/v1",
      "api": "openai-completions",
      "models": ["qwen3-coder:30b", "gpt-oss:20b"]
    },
    // ...
  ]
}
```

### Extra details

**Theme**: It can be a choice of `tokyo-night` or `auto`, defaults to tokyo. Auto is a theme that uses the host terminal theme. Plenty of CLIs do this, it shouldn't be a complicated thing. It should be just as detailed and accurate as tokyo-night.
**provider**: The provider id you want to use from one of the supported providers. Initially this is opencode-go and opencode-zen via an OPENCODE_API_KEY from the env. Or the id of one of your configured customProviders.
**model**: the model id for the specific model you want to use from the configured provider.
**effort**: pre-clamp, user configured value for effort. (models vary a lot on this, so the effective effort post clamp is shown in the TUI). This configures the desire of the user, not the effective value.
**custom providers**: api is openai-completions, openai-responses, anthropic-messages, or google-generative-ai. Then point provider and model at it.

## Agent core

Stays very similar in concept to mini-coder's with the exception of the tools.

The agent core needs to be interruptible, a polite request to pause at the next boundary for steering.
It also needs to be cancelable, and this needs to be responsive.

### Tools

A single, codemode style tool. https://blog.cloudflare.com/code-mode/ similar in concept to this. Instead of providing several tools, we provide a single code runtime, where we can expose configurable mcps, along with a runtime and standard library. QuickJS comes to mind as a nice embeddable option. Sandboxing is not a feature for now, but an embedded runtime would help us go this way in the future.
We currently don't plan to support MCP, but this is the foundation we need to do so eventually.
We render the call, and an elided body just like mini-coder does it, and we show the diff of the changes just like mini-coder does with bash calls.

- Capabilities: bash exec, read/write to file system, read images. (same as mini-coder but in code mode).

## Providers

We aim to support Opencode Zen, Opencode Go and local models via customProviders.

Instead of supporting all specific providers, we implement 4 modules, one for each major API format. This allows us to support the whole opencode catalog, without a specific implementation per model.
Model catalog: we should generate this from a reliable source so we have details about models, like context size, supported effort levels, etc.

We can look at the open-source pi-ai for reference.

## Headless mode

Prints the ongoing statuses to stderr, and tool activity (no tool bodies), and prints the final assistant response to stdout.
Can be used in script scenarios, pipes and input redirect, etc.
Accepts a `-p/--print` CLI argument as well.

## TUI

We are going to use the mini-coder UX/Appearance as a specification for how we render reasoning, tool bodies, user and assistant responses, errors, etc.

At startup as part of the banner, we should show the loaded agents file if any. Same for skills. So the user knows whats up right away.

Same commands as mini-coder, but with the addition of `/new` to start a new session.

### Themes

Our themes appear both in the chrome of the app, as well as syntax highlighting and diff rendering (see how
mini-coder does it). We use tree-sitter and their respective grammars. Then we follow `nvim`'s usage of it.
We intend to have this small set of themes:

- Tokyo Night (night variant).
- Auto (tries to use the host terminal's theme).

## Tech

Modern C11
tree-sitter and its grammars (nvim is a good reference I think, open source and also uses tree sitter).
