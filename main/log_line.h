/*
 * Lines from the ESP log, made fit for LOG (PROTOCOL.md 12): the level a line was
 * logged at, and its text without the console's decoration.
 *
 * Pure C, no IDF: the log bridge in main/link.c calls these, and
 * tests/host/test_rules.c walks them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "satlink/proto.h"

static inline bool log_line_escape_param(char c)
{
    return (c >= '0' && c <= '9') || c == ';';
}

/* Past one colour escape "\033[...m" at `p`, or `p` itself when there is none. */
static inline const char *log_line_skip_escape(const char *p)
{
    if (p[0] != '\033' || p[1] != '[') {
        return p;
    }
    const char *q = p + 2;
    while (log_line_escape_param(*q)) {
        q++;
    }
    return (*q == 'm') ? q + 1 : p;
}

/*
 * IDF log v1 prints "E (1234) tag: text", inside a colour escape when
 * CONFIG_LOG_COLORS is on, and the letter is the level: E, W, I, D -- and V, which
 * the protocol has no word for and which becomes debug. A line without a letter,
 * from a bare esp_log_write, is info.
 */
static inline uint8_t log_line_level(const char *line)
{
    const char *p = log_line_skip_escape(line);
    if (p[0] != '\0' && p[1] == ' ' && p[2] == '(') {
        switch (p[0]) {
        case 'E': return SATLINK_LOG_ERROR;
        case 'W': return SATLINK_LOG_WARN;
        case 'I': return SATLINK_LOG_INFO;
        case 'D':
        case 'V': return SATLINK_LOG_DEBUG;
        default:  break;
        }
    }
    return SATLINK_LOG_INFO;
}

/* True if text[esc..len) is an escape running to the end: whole, or cut short by
 * a buffer that was too small for the line. */
static inline bool log_line_escape_tail(const char *text, size_t esc, size_t len)
{
    size_t i = esc + 1;
    if (i == len) {
        return true;
    }
    if (text[i++] != '[') {
        return false;
    }
    while (i < len && log_line_escape_param(text[i])) {
        i++;
    }
    return i == len || (text[i] == 'm' && i + 1 == len);
}

/*
 * The line without the colour escape in front and the reset and newline behind:
 * the host wants text, not terminal control. Cuts `line` in place and returns
 * where the text starts.
 */
static inline char *log_line_tidy(char *line)
{
    char *text = (char *)log_line_skip_escape(line);
    size_t len = strlen(text);
    for (;;) {
        if (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
            len--;
            continue;
        }
        /* The last ESC, if it is close enough to the end to start the reset. */
        size_t esc = len;
        while (esc > 0 && len - esc < 12 && text[esc - 1] != '\033') {
            esc--;
        }
        if (esc > 0 && text[esc - 1] == '\033' && log_line_escape_tail(text, esc - 1, len)) {
            len = esc - 1;
            continue;
        }
        break;
    }
    text[len] = '\0';
    return text;
}
