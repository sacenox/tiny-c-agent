// cage - config-first agent for the terminal.

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
#include "tool.h"

static volatile bool g_cancel;

static void on_signal(int sig) {
    (void)sig;
    g_cancel = true;
}

static char *read_stream(FILE *f) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) abort();
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) abort();
        }
    }
    buf[len] = '\0';
    return buf;
}

static char *read_file(const char *path, char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        size_t n = strlen(path) + 32;
        *err = malloc(n);
        if (*err) snprintf(*err, n, "cannot read %s", path);
        return NULL;
    }
    char *s = read_stream(f);
    fclose(f);
    return s;
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

    if (!print_mode) {
        fprintf(stderr, "interactive mode is not implemented; use -p PROMPT\n");
        return 2;
    }

    char *stdin_prompt = NULL;
    if (!prompt) {
        if (isatty(fileno(stdin))) {
            fprintf(stderr, "-p requires a prompt\n");
            return 2;
        }
        stdin_prompt = read_stream(stdin);
        prompt = stdin_prompt;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *err = NULL;
    cage_config *cfg = cage_config_load(config_path, &err);
    if (!cfg) {
        fprintf(stderr, "%s\n", err ? err : "config error");
        return 1;
    }

    cage_model model;
    if (cage_resolve_model(cfg, &model, &err) != 0) {
        fprintf(stderr, "%s\n", err ? err : "model error");
        return 1;
    }

    char *system_prompt = NULL;
    if (cfg->system_prompt && cfg->system_prompt[0]) {
        system_prompt = read_file(cfg->system_prompt, &err);
        if (!system_prompt) {
            fprintf(stderr, "%s\n", err ? err : "prompt error");
            return 1;
        }
    } else {
        system_prompt = strdup("");
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");

    cage_session *session = cage_session_open(cfg->sessions_dir, cwd, prompt, &err);
    if (!session) {
        fprintf(stderr, "%s\n", err ? err : "session error");
        return 1;
    }

    cage_runtime *runtime = cage_runtime_new();
    cage_tool tool = cage_codemode_tool();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    cage_headless head;
    cage_headless_init(&head, stdout, stderr);

    cage_agent a = {0};
    a.model = model;
    a.system_prompt = system_prompt;
    a.tools = &tool;
    a.tools_len = 1;
    a.effort = cfg->effort;
    a.runtime = runtime;
    a.session = session;
    a.on_event = cage_headless_event;
    a.user = &head;
    a.cancel = &g_cancel;

    cage_agent_turn(&a, prompt);

    int code = cage_headless_finish(&head);

    cage_agent_free(&a);
    cage_headless_free(&head);
    cage_runtime_free(runtime);
    cage_session_close(session);
    free(system_prompt);
    cage_config_free(cfg);
    free(err);
    free(stdin_prompt);
    curl_global_cleanup();
    return code;
}

