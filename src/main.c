// tiny-c-agent - config-first agent for the terminal.

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "agent.h"
#include "config.h"
#include "headless.h"
#include "resolve.h"
#include "resources.h"
#include "tool.h"
#include "util.h"
#include "tui/tui.h"

static volatile bool g_cancel;

static void on_signal(int sig) {
    (void)sig;
    g_cancel = true;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *prompt = NULL;
    bool print_mode = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s requires a path\n", arg);
                return 2;
            }
            config_path = argv[i];
        } else if (strncmp(arg, "--config=", 9) == 0) {
            config_path = arg + 9;
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--print") == 0) {
            print_mode = true;
            if (i + 1 < argc) prompt = argv[++i];
        } else if (strncmp(arg, "--print=", 8) == 0) {
            print_mode = true;
            prompt = arg + 8;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            return 2;
        }
    }

    char *stdin_prompt = NULL;
    if (print_mode && !prompt) {
        if (isatty(fileno(stdin))) {
            fprintf(stderr, "-p requires a prompt\n");
            return 2;
        }
        stdin_prompt = tiny_read_stream(stdin);
        prompt = stdin_prompt;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *err = NULL;
    tiny_config *cfg = tiny_config_load(config_path, &err);
    if (!cfg) {
        fprintf(stderr, "%s\n", err ? err : "config error");
        return 1;
    }

    tiny_model model;
    memset(&model, 0, sizeof model);
    if (tiny_resolve_model(cfg, &model, &err) != 0) {
        if (print_mode) {
            fprintf(stderr, "%s\n", err ? err : "model error");
            return 1;
        }
        // Keep the interactive TUI usable: the user can pick a provider with
        // /provider. The turn reports NO_MODEL until one is configured.
        free(err);
        err = NULL;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");

    tiny_resources *resources = tiny_resources_build(cfg, cwd, &err);
    if (!resources) {
        fprintf(stderr, "%s\n", err ? err : "prompt error");
        return 1;
    }

    size_t tools_len;
    const tiny_tool *tools = tiny_tools(model.supports_images, &tools_len);

    if (!print_mode) {
        // The TUI owns the terminal and its own signal handlers.
        tiny_tui_config tc = {
            .cfg = cfg,
            .model = model,
            .resources = resources,
            .cwd = cwd,
        };
        int code = tiny_tui_run(&tc);
        tiny_resources_free(resources);
        tiny_config_free(cfg);
        free(err);
        free(stdin_prompt);
        curl_global_cleanup();
        return code;
    }

    tiny_session *session = tiny_session_open(cfg->sessions_dir, cwd, prompt, &err);
    if (!session) {
        fprintf(stderr, "%s\n", err ? err : "session error");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    tiny_headless head;
    tiny_headless_init(&head, stdout, stderr);

    tiny_agent a = {0};
    a.model = model;
    a.system_prompt = resources->system_prompt;
    a.tools = tools;
    a.tools_len = tools_len;

    a.session = session;
    a.on_event = tiny_headless_event;
    a.user = &head;
    a.cancel = &g_cancel;

    tiny_agent_turn(&a, prompt);

    int code = tiny_headless_finish(&head);

    tiny_agent_free(&a);
    tiny_headless_free(&head);
    tiny_session_close(session);
    tiny_resources_free(resources);
    tiny_config_free(cfg);
    free(err);
    free(stdin_prompt);
    curl_global_cleanup();
    return code;
}

