#include "usage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "theme.h"

// Trims trailing zeros from a fixed-decimal expansion.
static void trim_zeros(char *s) {
    char *dot = strchr(s, '.');
    if (!dot) return;
    char *p = s + strlen(s);
    while (p > dot + 1 && p[-1] == '0') p--;
    if (p > dot && p[-1] == '.') p--;
    *p = '\0';
}

char *tiny_format_tokens(long n) {
    char tmp[48];
    if (n < 1000) {
        snprintf(tmp, sizeof tmp, "%ld", n);
    } else if (n < 1000000) {
        snprintf(tmp, sizeof tmp, "%.1fk", (double)n / 1000.0);
        trim_zeros(tmp);
    } else {
        snprintf(tmp, sizeof tmp, "%.2fM", (double)n / 1000000.0);
        trim_zeros(tmp);
    }
    return strdup(tmp);
}

char *tiny_context_usage_line(long used, long window, long max_tokens) {
    if (window <= 0) {
        char *t = tiny_format_tokens(used);
        tui_buf b;
        buf_init(&b);
        char label[64];
        snprintf(label, sizeof label, "ctx %s", t);
        fg_dim(&b, label);
        free(t);
        if (!b.data) return strdup("");
        return buf_steal(&b);
    }
    char *u = tiny_format_tokens(used);
    char *w = tiny_format_tokens(window);
    tui_buf b;
    buf_init(&b);
    if (max_tokens > 0 && used + max_tokens > window) {
        char line[128];
        snprintf(line, sizeof line, "ctx full · %s/%s", u, w);
        fg_red(&b, line);
    } else {
        long percent = (long)((double)used / (double)window * 100.0 + 0.5);
        char line[128];
        snprintf(line, sizeof line, "ctx %s/%s · %ld%%", u, w, percent);
        if (percent >= 85) fg_yellow(&b, line);
        else fg_dim(&b, line);
    }
    free(u);
    free(w);
    if (!b.data) return strdup("");
    return buf_steal(&b);
}
