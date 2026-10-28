#include "cmt_web_util.h"

#include <stdio.h>
#include <string.h>

static const size_t WEB_NAME_MAX_LEN = 100;

bool web_normalize_path(const char* in, char* out, size_t out_size) {
    if (in == nullptr || out == nullptr || out_size < 2 || in[0] != '/') {
        return false;
    }

    size_t o = 0;
    out[o++] = '/';

    const char* p = in;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char* seg = p;
        while (*p && *p != '/') {
            if (*p == '\\' || (unsigned char)*p < 0x20) {
                return false;
            }
            p++;
        }
        size_t len = (size_t)(p - seg);

        if (len == 1 && seg[0] == '.') {
            continue;
        }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            return false;
        }

        if (o > 1) {
            if (o + 1 >= out_size) {
                return false;
            }
            out[o++] = '/';
        }
        if (o + len >= out_size) {
            return false;
        }
        memcpy(out + o, seg, len);
        o += len;
    }
    out[o] = '\0';
    return true;
}

bool web_is_valid_name(const char* name) {
    if (name == nullptr) {
        return false;
    }
    size_t len = strlen(name);
    if (len == 0 || len > WEB_NAME_MAX_LEN) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c) != nullptr) {
            return false;
        }
    }
    // FAT では末尾の空白とピリオドは使えない
    if (name[len - 1] == ' ' || name[len - 1] == '.') {
        return false;
    }
    return true;
}

bool web_join_path(const char* dir, const char* name, char* out, size_t out_size) {
    if (dir == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    bool root = (strcmp(dir, "/") == 0);
    int n = snprintf(out, out_size, root ? "%s%s" : "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size;
}

bool web_parent_path(const char* path, char* out, size_t out_size) {
    if (path == nullptr || out == nullptr || out_size < 2 || path[0] != '/' ||
        strcmp(path, "/") == 0) {
        return false;
    }
    const char* last = strrchr(path, '/');
    size_t len = (size_t)(last - path);
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
        return true;
    }
    if (len >= out_size) {
        return false;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

bool web_path_is_within(const char* path, const char* base) {
    if (path == nullptr || base == nullptr) {
        return false;
    }
    if (strcmp(base, "/") == 0) {
        return path[0] == '/';
    }
    size_t n = strlen(base);
    return strncmp(path, base, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

const char* web_base_name(const char* path) {
    if (path == nullptr) {
        return "";
    }
    const char* name = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

size_t web_json_escape(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    for (const char* p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char tmp[8];
        const char* s = tmp;
        size_t len;
        if (c == '"') {
            s = "\\\"";
            len = 2;
        } else if (c == '\\') {
            s = "\\\\";
            len = 2;
        } else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            len = 6;
        } else {
            tmp[0] = (char)c;
            len = 1;
        }
        if (o + len >= out_size) {
            return (size_t)-1;
        }
        memcpy(out + o, s, len);
        o += len;
    }
    if (o >= out_size) {
        return (size_t)-1;
    }
    out[o] = '\0';
    return o;
}

size_t web_url_encode(const char* in, char* out, size_t out_size) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const char* p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || strchr("-._~", c) != nullptr;
        size_t len = plain ? 1 : 3;
        if (o + len >= out_size) {
            return (size_t)-1;
        }
        if (plain) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0x0F];
        }
    }
    if (o >= out_size) {
        return (size_t)-1;
    }
    out[o] = '\0';
    return o;
}
