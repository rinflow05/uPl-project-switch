/**
 * uPl-project-switch.c — ustxPlayer 的工程格式转换工具（uPl 项目的一部分）
 *
 * 拖放 .uplr 工程文件到此程序上，自动识别并转换为对应程序可读取的工程文件。
 * uPl 是 ustxPlayer 与 ustPlayer 两个程序共同的项目名缩写，
 * 本工具在两者之间转换工程格式。
 *
 * 编译: gcc -Os -s -static -o uPl-project-switch.exe uPl-project-switch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif

/* ===================== 动态缓冲区 ===================== */

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_init(Buf *b) {
    b->cap = 8192; b->len = 0;
    b->p = (char*)malloc(b->cap);
    b->p[0] = '\0';
}
static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        while (b->len + need + 1 > b->cap) b->cap *= 2;
        b->p = (char*)realloc(b->p, b->cap);
    }
}
static void buf_append(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}
static void buf_str(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }
static void buf_char(Buf *b, char c) { buf_append(b, &c, 1); }
static void buf_int(Buf *b, int v) {
    char t[32]; int n = snprintf(t, sizeof(t), "%d", v);
    buf_append(b, t, n);
}
static void buf_dbl(Buf *b, double v) {
    char t[32]; int n = snprintf(t, sizeof(t), "%.1f", v);
    buf_append(b, t, n);
}
static void buf_json_str(Buf *b, const char *s) {
    buf_char(b, '"');
    for (; *s; s++) {
        switch (*s) {
        case '"':  buf_str(b, "\\\""); break;
        case '\\': buf_str(b, "\\\\"); break;
        case '\n': buf_str(b, "\\n"); break;
        case '\r': buf_str(b, "\\r"); break;
        case '\t': buf_str(b, "\\t"); break;
        default:
            if ((unsigned char)*s < 0x20) {
                char t[8]; snprintf(t, sizeof(t), "\\u%04x", (unsigned char)*s);
                buf_str(b, t);
            } else buf_char(b, *s);
        }
    }
    buf_char(b, '"');
}

/* ===================== 文件 I/O（统一 UTF-8 路径） ===================== */

#ifdef _WIN32
/* UTF-8 路径 转 宽字符（供 _wfopen 使用，避免 ANSI 代码页与 UTF-8 冲突导致中文乱码/打不开） */
static const wchar_t *utf8_to_wide(const char *u8, wchar_t *buf, int buflen) {
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, buf, buflen) <= 0) return NULL;
    return buf;
}
/* 宽字符 转 UTF-8（用于打印和程序内部统一用 UTF-8） */
static char *wide_to_utf8(const wchar_t *w, char *buf, int buflen) {
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, buflen, NULL, NULL) <= 0) return NULL;
    return buf;
}
static FILE *pfopen(const char *path, const char *mode) {
    wchar_t wpath[4096], wmode[16];
    if (!utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wchar_t)))) return NULL;
    if (!utf8_to_wide(mode, wmode, (int)(sizeof(wmode) / sizeof(wchar_t)))) return NULL;
    return _wfopen(wpath, wmode);
}
#else
static FILE *pfopen(const char *path, const char *mode) { return fopen(path, mode); }
#endif

/* 文件是否存在（UTF-8 路径） */
static int file_exists(const char *path) {
    FILE *f = pfopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = pfopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(sz + 1);
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *f = pfopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

/* ===================== JSON 字段提取 ===================== */

/* 在 JSON 中找到 "key": "value"，返回 value 的起止指针（不含引号） */
static int find_json_string(const char *json, const char *key,
                             const char **start, const char **end) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += strlen(pattern);
    /* 跳过 : 和空白 */
    pos = strchr(pos, ':');
    if (!pos) return 0;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != '"') return 0;
    pos++;
    *start = pos;
    /* 找到未转义的结束引号 */
    while (*pos) {
        if (*pos == '\\' && pos[1]) pos += 2;
        else if (*pos == '"') { *end = pos; return 1; }
        else pos++;
    }
    return 0;
}

/* 在 JSON 中找到 "key": <number>，返回 number 的起止指针 */
static int find_json_value(const char *json, const char *key,
                            const char **start, const char **end) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += strlen(pattern);
    pos = strchr(pos, ':');
    if (!pos) return 0;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    *start = pos;
    while (*pos && *pos != ',' && *pos != '}' && *pos != ']' &&
           *pos != '\n' && *pos != '\r') pos++;
    /* 去尾部空白 */
    while (pos > *start && (pos[-1] == ' ' || pos[-1] == '\t')) pos--;
    *end = pos;
    return (pos > *start);
}

/* 提取 JSON 字符串值（unescape）到新分配的内存 */
static char *extract_json_string(const char *json, const char *key) {
    const char *s, *e;
    if (!find_json_string(json, key, &s, &e)) return NULL;
    size_t len = e - s;
    char *out = (char*)malloc(len + 1);
    size_t j = 0;
    for (const char *p = s; p < e; p++) {
        if (*p == '\\' && p + 1 < e) {
            switch (p[1]) {
            case '"': out[j++] = '"'; p++; break;
            case '\\': out[j++] = '\\'; p++; break;
            case 'n': out[j++] = '\n'; p++; break;
            case 'r': out[j++] = '\r'; p++; break;
            case 't': out[j++] = '\t'; p++; break;
            case '/': out[j++] = '/'; p++; break;
            case 'u': {
                if (p + 5 < e) {
                    unsigned int cp = 0;
                    sscanf(p + 2, "%4x", &cp);
                    p += 4;
                    /* 高低代理配对（\uDXXX\uDYYY）还原成单个码点，避免 emoji 等乱码 */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                        unsigned int lo = 0;
                        sscanf(p + 3, "%4x", &lo);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    if (cp < 0x80) out[j++] = (char)cp;
                    else if (cp < 0x800) {
                        out[j++] = (char)(0xC0 | (cp >> 6));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out[j++] = (char)(0xE0 | (cp >> 12));
                        out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[j++] = (char)(0xF0 | (cp >> 18));
                        out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default: out[j++] = p[1]; p++; break;
            }
        } else out[j++] = *p;
    }
    out[j] = '\0';
    return out;
}

/* 提取 settings JSON 对象（大括号匹配） */
static char *extract_settings(const char *json) {
    const char *pos = strstr(json, "\"settings\"");
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != '{') return NULL;
    int depth = 0;
    const char *start = pos;
    while (*pos) {
        if (*pos == '"') {
            pos++;
            while (*pos) {
                if (*pos == '\\' && pos[1]) pos += 2;
                else if (*pos == '"') { pos++; break; }
                else pos++;
            }
        } else if (*pos == '{') { depth++; pos++; }
        else if (*pos == '}') { depth--; pos++; if (depth == 0) break; }
        else pos++;
    }
    size_t len = pos - start;
    char *out = (char*)malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* ===================== YAML 解析 ===================== */

/* 获取行首缩进空格数 */
static int indent_level(const char *line) {
    int n = 0;
    while (line[n] == ' ') n++;
    return n;
}

/* 跳过行首缩进和可选的 "- "，返回内容起始指针 */
static const char *skip_indent_item(const char *line) {
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '-' && (p[1] == ' ' || p[1] == '\0')) p += 2;
    while (*p == ' ') p++;
    return p;
}

/* 检查行是否是注释或空行 */
static int is_blank_or_comment(const char *line) {
    while (*line == ' ') line++;
    return (*line == '\0' || *line == '\n' || *line == '\r' || *line == '#');
}

/* 从 "key: value" 行提取 value 部分指针（去除前后空白） */
static const char *get_yaml_value(const char *line) {
    const char *p = skip_indent_item(line);
    const char *colon = strchr(p, ':');
    if (!colon) return NULL;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    /* 去尾部换行 */
    const char *end = colon;
    while (*end && *end != '\n' && *end != '\r') end++;
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end == colon) return NULL;
    /* 返回指向 null 结尾的临时 — 需要调用方处理 */
    return colon;
}

/* 从 YAML 行提取 key 和 value（key 拷贝到 buf，value 返回指针） */
static int parse_yaml_kv(const char *line, char *key_buf, int key_size,
                          const char **val_start, int *val_len) {
    const char *p = skip_indent_item(line);
    const char *colon = strchr(p, ':');
    if (!colon) return 0;
    int klen = colon - p;
    if (klen >= key_size) klen = key_size - 1;
    memcpy(key_buf, p, klen);
    key_buf[klen] = '\0';
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    const char *end = colon;
    while (*end && *end != '\n' && *end != '\r') end++;
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *val_start = colon;
    *val_len = end - colon;
    return 1;
}

/* 从 YAML 值字符串提取引号内容 */
static void extract_yaml_quoted(const char *val, int vlen, char *out, int out_size) {
    if (vlen >= 2 && (val[0] == '"' || val[0] == '\'') && val[vlen-1] == val[0]) {
        int n = vlen - 2;
        if (n >= out_size) n = out_size - 1;
        memcpy(out, val + 1, n);
        out[n] = '\0';
    } else {
        int n = vlen;
        if (n >= out_size) n = out_size - 1;
        memcpy(out, val, n);
        out[n] = '\0';
    }
}

/* 从 flow mapping 如 "{x: -40, y: 0, shape: io}" 提取指定 key 的 int 值 */
static int flow_get_int(const char *line, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s:", key);
    const char *p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

/* ===================== 数据结构 ===================== */

typedef struct { int position; int bpm; } Tempo;

typedef struct {
    int track_no;
    char track_name[128];
    int note_count;
} TrackInfo;

typedef struct {
    int position;
    int length;
    char lyric[512];
    int note_num;
    int track_no;
    int *pitch_bend;
    int *pitch_ticks;
    int pitch_count;
} Note;

typedef struct {
    char version[32];
    double tempo;
    int tempo_count;
    Tempo *tempos;
    int tempo_total;
    int tracks;
    TrackInfo *tracks_info;      /* 人声轨（仅有音符） */
    int tracks_info_count;
    TrackInfo *empty_tracks;     /* 空人声轨（无音符，且非音频轨） */
    int empty_tracks_count;
    TrackInfo *all_tracks;       /* 全部轨道（含空轨/音频轨） */
    int all_tracks_count;
    char *wave_flag;             /* 长度 = tracks，1 表示被 wave_parts 引用（音频轨） */
    int wave_part_count;
    Note *notes;
    int note_count;
    int note_cap;
} UstxData;

static void udata_init(UstxData *d) {
    memset(d, 0, sizeof(*d));
    strcpy(d->version, "unknown");
    d->tempo = 120.0;
    d->note_cap = 256;
    d->notes = (Note*)calloc(d->note_cap, sizeof(Note));
    d->note_count = 0;
    d->tempo_total = 16;
    d->tempos = (Tempo*)calloc(d->tempo_total, sizeof(Tempo));
    d->tempo_count = 0;
    d->tracks_info_count = 0;
    d->tracks_info = NULL;
}

static Note *udata_new_note(UstxData *d) {
    if (d->note_count >= d->note_cap) {
        d->note_cap *= 2;
        d->notes = (Note*)realloc(d->notes, d->note_cap * sizeof(Note));
        memset(d->notes + d->note_count, 0,
               (d->note_cap - d->note_count) * sizeof(Note));
    }
    return &d->notes[d->note_count++];
}

/* ===================== YAML 行解析器 ===================== */

/* 将 YAML 文本按行分割，返回行数和行指针数组 */
static char **split_lines(const char *text, int *out_count) {
    int count = 0, cap = 1024;
    char **lines = (char**)malloc(cap * sizeof(char*));
    const char *p = text;
    while (*p) {
        const char *start = p;
        while (*p && *p != '\n') p++;
        size_t len = p - start;
        if (*p == '\n') p++;
        /* 去除 \r（Windows 换行） */
        if (len > 0 && start[len-1] == '\r') len--;
        char *line = (char*)malloc(len + 1);
        memcpy(line, start, len);
        line[len] = '\0';
        if (count >= cap) {
            cap *= 2;
            lines = (char**)realloc(lines, cap * sizeof(char*));
        }
        lines[count++] = line;
    }
    *out_count = count;
    return lines;
}

/* 查找指定 key 所在行（从 start_line 开始，限定缩进 <= max_indent） */
static int find_key_line(char **lines, int total, int start, const char *key, int max_indent) {
    for (int i = start; i < total; i++) {
        if (is_blank_or_comment(lines[i])) continue;
        int ind = indent_level(lines[i]);
        if (ind > max_indent) continue;
        if (ind == 0 && i > start && !is_blank_or_comment(lines[i])) {
            /* 到了下一个顶级 section，停止 */
            const char *content = skip_indent_item(lines[i]);
            char k[128]; const char *v; int vl;
            if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
                if (strcmp(k, key) == 0) return i;
            }
            /* 不是我们要找的 key，继续找 */
        }
        const char *content = skip_indent_item(lines[i]);
        char k[128]; const char *v; int vl;
        if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
            if (strcmp(k, key) == 0) return i;
        }
    }
    return -1;
}

/* 查找顶级 key 所在行 */
static int find_root_key(char **lines, int total, const char *key) {
    for (int i = 0; i < total; i++) {
        if (is_blank_or_comment(lines[i])) continue;
        if (indent_level(lines[i]) != 0) continue;
        const char *content = skip_indent_item(lines[i]);
        char k[128]; const char *v; int vl;
        if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
            if (strcmp(k, key) == 0) return i;
        }
    }
    return -1;
}

/* 查找下一个顶级 key 所在行（在指定行之后，跳过列表项） */
static int find_next_root(char **lines, int total, int after) {
    for (int i = after + 1; i < total; i++) {
        if (is_blank_or_comment(lines[i])) continue;
        if (indent_level(lines[i]) == 0) {
            const char *content = lines[i];
            while (*content == ' ') content++;
            if (*content == '-') continue; /* 跳过列表项 */
            if (strchr(content, ':')) return i;
        }
    }
    return total;
}

/* ===================== USTX 解析主逻辑 ===================== */

static void parse_ustx(const char *yaml, UstxData *d) {
    int total_lines;
    char **lines = split_lines(yaml, &total_lines);

    /* 1. ustx_version */
    int li = find_root_key(lines, total_lines, "ustx_version");
    if (li >= 0) {
        char k[128]; const char *v; int vl;
        if (parse_yaml_kv(lines[li], k, sizeof(k), &v, &vl)) {
            extract_yaml_quoted(v, vl, d->version, sizeof(d->version));
        }
    }

    /* 2. tempos */
    li = find_root_key(lines, total_lines, "tempos");
    if (li >= 0) {
        int next_root = find_next_root(lines, total_lines, li);
        for (int i = li + 1; i < next_root; i++) {
            if (is_blank_or_comment(lines[i])) continue;
            int ind = indent_level(lines[i]);
            if (lines[i][ind] == '-') {
                /* 新 tempo 项（dash 可能在列 0，如 "- position: 0"） */
                if (d->tempo_count >= d->tempo_total) {
                    d->tempo_total *= 2;
                    d->tempos = (Tempo*)realloc(d->tempos, d->tempo_total * sizeof(Tempo));
                }
                Tempo *t = &d->tempos[d->tempo_count];
                t->position = 0; t->bpm = 120;
                /* 同行可能有 position: 0 */
                const char *content = skip_indent_item(lines[i]);
                char k[128]; const char *v; int vl;
                if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
                    if (strcmp(k, "position") == 0) t->position = atoi(v);
                    else if (strcmp(k, "bpm") == 0) t->bpm = atoi(v);
                }
                d->tempo_count++;
            } else if (ind > 0) {
                /* tempo 字段 */
                char k[128]; const char *v; int vl;
                if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
                    if (d->tempo_count > 0) {
                        Tempo *t = &d->tempos[d->tempo_count - 1];
                        if (strcmp(k, "position") == 0) t->position = atoi(v);
                        else if (strcmp(k, "bpm") == 0) t->bpm = atoi(v);
                    }
                }
            }
        }
    }
    if (d->tempo_count > 0) d->tempo = (double)d->tempos[0].bpm;
    else {
        li = find_root_key(lines, total_lines, "bpm");
        if (li >= 0) {
            char k[128]; const char *v; int vl;
            if (parse_yaml_kv(lines[li], k, sizeof(k), &v, &vl)) d->tempo = atof(v);
        }
    }

    /* 3. tracks (count + names) */
    li = find_root_key(lines, total_lines, "tracks");
    if (li >= 0) {
        int next_root = find_next_root(lines, total_lines, li);
        int track_idx = 0;        /* 已见到的轨道数量 */
        int cur_track = -1;       /* 当前轨道下标（0 起，与 voice_parts 的 track_no 对齐） */
        int *name_owner = NULL;   /* names[i] 归属的轨道下标 */
        char **names = NULL;
        int name_count = 0;
        /* 先收集 track_name，并记录其归属的轨道下标，
           避免部分轨道缺名时把名字错位塞进别的轨道（参照 ustxreader 按下标取名） */
        for (int i = li + 1; i < next_root; i++) {
            if (is_blank_or_comment(lines[i])) continue;
            int ind = indent_level(lines[i]);
            /* 过滤不可解析的轨道：track 列表项 dash 在列 0（如 "- singer: ..."）。
                                   嵌套列表项（dash 在缩进>0，如 singer 子模型、phonemizer 参数）
                                   不是独立轨道，必须排除，否则会被误计。 */
            if (ind == 0 && lines[i][ind] == '-') {
                cur_track = track_idx;
                track_idx++;
            } else if (ind > 0 && cur_track >= 0) {
                char k[128]; const char *v; int vl;
                if (parse_yaml_kv(lines[i], k, sizeof(k), &v, &vl)) {
                    if (strcmp(k, "track_name") == 0 || strcmp(k, "name") == 0) {
                        char name[128];
                        extract_yaml_quoted(v, vl, name, sizeof(name));
                        name_count++;
                        names = (char**)realloc(names, name_count * sizeof(char*));
                        name_owner = (int*)realloc(name_owner, name_count * sizeof(int));
                        name_owner[name_count - 1] = cur_track;
                        names[name_count - 1] = strdup(name);
                    }
                }
            }
        }
        d->tracks = track_idx > 0 ? track_idx : 1;
        d->tracks_info = (TrackInfo*)calloc(d->tracks, sizeof(TrackInfo));
        for (int i = 0; i < d->tracks; i++) {
            d->tracks_info[i].track_no = i;
            int found = 0;
            for (int ni = 0; ni < name_count; ni++) {
                if (name_owner[ni] == i) {
                    strncpy(d->tracks_info[i].track_name, names[ni], 127);
                    d->tracks_info[i].track_name[127] = '\0';
                    found = 1;
                    break;
                }
            }
            if (!found) snprintf(d->tracks_info[i].track_name, 128, "轨道 %d", i + 1);
            d->tracks_info[i].note_count = 0;
        }
        for (int ni = 0; ni < name_count; ni++) free(names[ni]);
        free(names); free(name_owner);
    } else d->tracks = 1;

    /* 4. wave_parts count + wave track flags（音频轨） */
    if (d->tracks > 0) d->wave_flag = (char*)calloc(d->tracks, 1);
    li = find_root_key(lines, total_lines, "wave_parts");
    if (li >= 0) {
        char k[128]; const char *v; int vl;
        if (parse_yaml_kv(lines[li], k, sizeof(k), &v, &vl)) {
            /* 可能是 "[]" 或列表 */
            if (vl >= 2 && v[0] == '[' && v[1] == ']') d->wave_part_count = 0;
            else {
                int next_root = find_next_root(lines, total_lines, li);
                int count = 0;
                int cur_track = -1;
                for (int i = li + 1; i < next_root; i++) {
                    if (is_blank_or_comment(lines[i])) continue;
                    int ind = indent_level(lines[i]);
                    if (ind == 0 && lines[i][ind] == '-') {
                        /* 新 wave_part 开始 */
                        count++;
                        cur_track = -1;
                        /* 同行可能有 track_no */
                        char ck[128]; const char *cv; int cvl;
                        if (parse_yaml_kv(lines[i], ck, sizeof(ck), &cv, &cvl)) {
                            if (strcmp(ck, "track_no") == 0) cur_track = atoi(cv);
                        }
                        if (cur_track >= 0 && d->wave_flag && cur_track < d->tracks)
                            d->wave_flag[cur_track] = 1;
                    } else if (ind == 2) {
                        /* wave_part 级字段 */
                        char ck[128]; const char *cv; int cvl;
                        if (parse_yaml_kv(lines[i], ck, sizeof(ck), &cv, &cvl)) {
                            if (strcmp(ck, "track_no") == 0) {
                                cur_track = atoi(cv);
                                if (cur_track >= 0 && d->wave_flag && cur_track < d->tracks)
                                    d->wave_flag[cur_track] = 1;
                            }
                        }
                    }
                }
                d->wave_part_count = count;
            }
        }
    }

    /* 5. voice_parts → notes + curves */
    li = find_root_key(lines, total_lines, "voice_parts");
    if (li >= 0) {
        int next_root = find_next_root(lines, total_lines, li);
        int i = li + 1;
        while (i < next_root) {
            if (is_blank_or_comment(lines[i])) { i++; continue; }
            int ind = indent_level(lines[i]);
            if (ind == 0 && lines[i][ind] == '-') {
                /* 新 voice_part 开始 */
                int part_indent = 0; /* part 内字段缩进为 2 */
                int part_pos = 0;
                int track_no = 0;
                int part_end = next_root;

                /* 找到 part 结束（下一个 - at column 0 或下一个 root key） */
                for (int j = i + 1; j < next_root; j++) {
                    if (is_blank_or_comment(lines[j])) continue;
                    int ji = indent_level(lines[j]);
                    if (ji == 0) { part_end = j; break; }
                }

                /* 解析 part 字段（仅 indent 2 且非列表项，避免误扫 notes 列表覆写 part_pos） */
                int notes_line = -1, curves_line = -1;
                for (int j = i; j < part_end; j++) {
                    if (is_blank_or_comment(lines[j])) continue;
                    int ji = indent_level(lines[j]);
                    if (ji != 2) continue;  /* 只处理 part 级字段（indent 2） */
                    if (lines[j][ji] == '-') continue;  /* 跳过列表项（notes/curves 条目） */
                    char k[128]; const char *v; int vl;
                    if (parse_yaml_kv(lines[j], k, sizeof(k), &v, &vl)) {
                        if (strcmp(k, "track_no") == 0) track_no = atoi(v);
                        else if (strcmp(k, "position") == 0) part_pos = atoi(v);
                        else if (strcmp(k, "notes") == 0) notes_line = j;
                        else if (strcmp(k, "curves") == 0) curves_line = j;
                    }
                }

                /* 解析 pitd 曲线 */
                int *pitd_xs = NULL, *pitd_ys = NULL;
                int pitd_count = 0;
                if (curves_line >= 0) {
                    int curves_end = part_end;
                    /* 找 curves 结束 */
                    for (int j = curves_line + 1; j < part_end; j++) {
                        if (is_blank_or_comment(lines[j])) continue;
                        int ji = indent_level(lines[j]);
                        if (ji <= 2 && lines[j][ji] != '-') {
                            /* 同级或更高级的 key，不是 curves 内容 */
                            if (ji <= 2) { curves_end = j; break; }
                        }
                    }
                    /* 遍历 curves 中的每个 curve */
                    int j = curves_line + 1;
                    while (j < curves_end) {
                        if (is_blank_or_comment(lines[j])) { j++; continue; }
                        int ji = indent_level(lines[j]);
                        if (ji == 2 && lines[j][ji] == '-') {
                            /* 新 curve */
                            int curve_end = curves_end;
                            for (int k2 = j + 1; k2 < curves_end; k2++) {
                                if (is_blank_or_comment(lines[k2])) continue;
                                int ki = indent_level(lines[k2]);
                                if (ki <= 2 && lines[k2][ki] == '-') { curve_end = k2; break; }
                            }
                            char abbr[32] = "";
                            int *xs = NULL, *ys = NULL;
                            int xs_count = 0, ys_count = 0;
                            int in_xs = 0, in_ys = 0;
                            for (int k2 = j; k2 < curve_end; k2++) {
                                if (is_blank_or_comment(lines[k2])) continue;
                                char k[128]; const char *v; int vl;
                                if (parse_yaml_kv(lines[k2], k, sizeof(k), &v, &vl)) {
                                    if (strcmp(k, "abbr") == 0)
                                        extract_yaml_quoted(v, vl, abbr, sizeof(abbr));
                                    else if (strcmp(k, "xs") == 0) {
                                        in_xs = 1; in_ys = 0;
                                        /* 检查是否同行有 [] */
                                        if (vl >= 2 && v[0] == '[' && v[1] == ']') in_xs = 0;
                                    } else if (strcmp(k, "ys") == 0) {
                                        in_ys = 1; in_xs = 0;
                                        if (vl >= 2 && v[0] == '[' && v[1] == ']') in_ys = 0;
                                    }
                                } else {
                                    /* 列表项 */
                                    int ki = indent_level(lines[k2]);
                                    const char *content = skip_indent_item(lines[k2]);
                                    if (in_xs && content[0] != '\0') {
                                        xs_count++;
                                        xs = (int*)realloc(xs, xs_count * sizeof(int));
                                        xs[xs_count - 1] = atoi(content);
                                    } else if (in_ys && content[0] != '\0') {
                                        ys_count++;
                                        ys = (int*)realloc(ys, ys_count * sizeof(int));
                                        ys[ys_count - 1] = atoi(content);
                                    }
                                }
                            }
                            /* 如果是 pitd 曲线，保存 */
                            if (strcmp(abbr, "pitd") == 0) {
                                pitd_count = xs_count < ys_count ? xs_count : ys_count;
                                if (pitd_count > 0) {
                                    pitd_xs = (int*)malloc(pitd_count * sizeof(int));
                                    pitd_ys = (int*)malloc(pitd_count * sizeof(int));
                                    memcpy(pitd_xs, xs, pitd_count * sizeof(int));
                                    memcpy(pitd_ys, ys, pitd_count * sizeof(int));
                                }
                            }
                            free(xs); free(ys);
                            j = curve_end;
                        } else j++;
                    }
                }

                /* 解析 notes */
                if (notes_line >= 0) {
                    int notes_end = curves_line >= 0 ? curves_line : part_end;
                    /* 如果 curves 在 notes 之前，notes_end 是 part_end */
                    if (curves_line >= 0 && curves_line < notes_line) {
                        notes_end = part_end;
                    } else if (curves_line >= 0) {
                        notes_end = curves_line;
                    }
                    int note_global_idx = d->note_count;
                    int j = notes_line + 1;
                    while (j < notes_end) {
                        if (is_blank_or_comment(lines[j])) { j++; continue; }
                        int ji = indent_level(lines[j]);
                        if (ji == 2 && lines[j][ji] == '-') {
                            /* 新 note */
                            int note_end = notes_end;
                            for (int k2 = j + 1; k2 < notes_end; k2++) {
                                if (is_blank_or_comment(lines[k2])) continue;
                                int ki = indent_level(lines[k2]);
                                if (ki <= 2 && lines[k2][ki] == '-') { note_end = k2; break; }
                            }
                            /* 过滤不可解析的轨道：track_no 越界（超出 tracks 有效范围）时
                               无法关联到有效轨道，跳过该部分的 note，避免生成无效数据 */
                            if (track_no < 0 || track_no >= d->tracks) {
                                j = note_end;
                                continue;
                            }
                            Note *note = udata_new_note(d);
                            note->track_no = track_no;
                            note->note_num = 0;
                            note->position = 0;
                            note->length = 0;
                            note->lyric[0] = '\0';
                            note->pitch_bend = NULL;
                            note->pitch_ticks = NULL;
                            note->pitch_count = 0;

                            int note_pos_val = 0, note_dur = 0;
                            for (int k2 = j; k2 < note_end; k2++) {
                                if (is_blank_or_comment(lines[k2])) continue;
                                char k[128]; const char *v; int vl;
                                if (parse_yaml_kv(lines[k2], k, sizeof(k), &v, &vl)) {
                                    if (strcmp(k, "position") == 0) note_pos_val = atoi(v);
                                    else if (strcmp(k, "duration") == 0) note_dur = atoi(v);
                                    else if (strcmp(k, "tone") == 0) note->note_num = atoi(v);
                                    else if (strcmp(k, "lyric") == 0) {
                                        char lyric[512];
                                        extract_yaml_quoted(v, vl, lyric, sizeof(lyric));
                                        /* 去语言前缀 */
                                        char *slash = strchr(lyric, '/');
                                        if (slash) strncpy(note->lyric, slash + 1, 511);
                                        else strncpy(note->lyric, lyric, 511);
                                        note->lyric[511] = '\0';
                                    }
                                }
                            }
                            note->position = part_pos + note_pos_val;
                            note->length = note_dur;

                            /* 计算 pitch_bend */
                            if (pitd_count > 0) {
                                /* pitd_xs 是相对 voice_part 的坐标，需与 part 内相对位置比较 */
                                int note_start = note->position - part_pos;
                                int note_end_tick = note_start + note->length;
                                int pb_cap = 16, pb_count = 0;
                                int *pb = (int*)malloc(pb_cap * sizeof(int));
                                int *pt = (int*)malloc(pb_cap * sizeof(int));
                                for (int p2 = 0; p2 < pitd_count; p2++) {
                                    if (pitd_xs[p2] >= note_start && pitd_xs[p2] <= note_end_tick) {
                                        if (pb_count >= pb_cap) {
                                            pb_cap *= 2;
                                            pb = (int*)realloc(pb, pb_cap * sizeof(int));
                                            pt = (int*)realloc(pt, pb_cap * sizeof(int));
                                        }
                                        pb[pb_count] = pitd_ys[p2];
                                        pt[pb_count] = pitd_xs[p2] - note_start;
                                        pb_count++;
                                    }
                                }
                                note->pitch_bend = pb;
                                note->pitch_ticks = pt;
                                note->pitch_count = pb_count;
                            }

                            /* 更新 track note count */
                            for (int t = 0; t < d->tracks; t++) {
                                if (d->tracks_info[t].track_no == track_no) {
                                    d->tracks_info[t].note_count++;
                                    break;
                                }
                            }
                            j = note_end;
                        } else j++;
                    }
                }

                free(pitd_xs); free(pitd_ys);
                i = part_end;
            } else i++;
        }
    }

    /* 保存全部轨道（含空轨/音频轨），供 empty_tracks 使用 */
    if (d->tracks > 0) {
        d->all_tracks = (TrackInfo*)calloc(d->tracks, sizeof(TrackInfo));
        d->all_tracks_count = d->tracks;
        for (int t = 0; t < d->tracks; t++) d->all_tracks[t] = d->tracks_info[t];
    }

    /* 构建 tracks_info（只含有音符的轨道） */
    int vocal_count = 0;
    for (int t = 0; t < d->tracks; t++) {
        if (d->tracks_info[t].note_count > 0) vocal_count++;
    }
    if (vocal_count > 0) {
        TrackInfo *new_info = (TrackInfo*)malloc(vocal_count * sizeof(TrackInfo));
        int idx = 0;
        for (int t = 0; t < d->tracks; t++) {
            if (d->tracks_info[t].note_count > 0) {
                new_info[idx++] = d->tracks_info[t];
            }
        }
        free(d->tracks_info);
        d->tracks_info = new_info;
        d->tracks_info_count = vocal_count;
    } else {
        d->tracks_info_count = 0;
    }

    /* 构建 empty_tracks（无音符，且非音频轨） */
    int empty_count = 0;
    if (d->all_tracks_count > 0) {
        for (int t = 0; t < d->all_tracks_count; t++) {
            if (d->all_tracks[t].note_count == 0 &&
                !(d->wave_flag && d->wave_flag[d->all_tracks[t].track_no])) empty_count++;
        }
        if (empty_count > 0) {
            d->empty_tracks = (TrackInfo*)calloc(empty_count, sizeof(TrackInfo));
            int idx = 0;
            for (int t = 0; t < d->all_tracks_count; t++) {
                if (d->all_tracks[t].note_count == 0 &&
                    !(d->wave_flag && d->wave_flag[d->all_tracks[t].track_no])) {
                    d->empty_tracks[idx++] = d->all_tracks[t];
                }
            }
            d->empty_tracks_count = empty_count;
        }
    }

    /* 释放行内存 */
    for (int i = 0; i < total_lines; i++) free(lines[i]);
    free(lines);
}

/* ===================== settings 规范化重写 ===================== */

/* 跳过 p 处的一个 JSON 值，返回其后已闭合的指针（p 指向值起点：'"'/'{ ' '['/标量） */
static const char *json_skip_value(const char *p) {
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\' && p[1]) p += 2;
            else if (*p == '"') { p++; break; }
            else p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0; const char *q = p;
        while (*q) {
            if (*q == '"') {
                q++;
                while (*q) {
                    if (*q == '\\' && q[1]) q += 2;
                    else if (*q == '"') { q++; break; }
                    else q++;
                }
            } else {
                if (*q == open) depth++;
                else if (*q == close) {
                    depth--; q++;
                    if (depth == 0) return q;
                    continue;
                }
            }
            q++;
        }
        return q;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 读取对象键（p 指向开引号），拷贝到 key，返回收尾引号后指针 */
static const char *json_read_key(const char *p, char *key, int ksize) {
    p++; int j = 0;
    while (*p) {
        if (*p == '\\' && p[1]) { if (j < ksize - 1) key[j++] = p[1]; p += 2; }
        else if (*p == '"') { p++; break; }
        else { if (j < ksize - 1) key[j++] = *p; p++; }
    }
    key[j] = '\0';
    return p;
}

/* 对象条目：key + 原始值区间，type: 0=标量 1=字符串 2=对象 3=数组 */
typedef struct {
    char key[128];
    const char *vstart, *vend;
    int type;
} SEntry;

/* 解析 `{...}` 对象的所有顶级条目到数组；返回条目数（obj 指向 '{'） */
static int split_object_entries(const char *obj, SEntry **out) {
    SEntry *e = NULL; int n = 0, cap = 0;
    const char *p = obj;
    if (*p == '{') p++;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '}') break;
        if (*p != '"') { break; }
        if (n >= cap) { cap = cap ? cap * 2 : 16; e = (SEntry*)realloc(e, cap * sizeof(SEntry)); }
        p = json_read_key(p, e[n].key, sizeof(e[n].key));
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        e[n].vstart = p;
        if (*p == '"') e[n].type = 1;
        else if (*p == '{') e[n].type = 2;
        else if (*p == '[') e[n].type = 3;
        else e[n].type = 0;
        p = json_skip_value(p);
        e[n].vend = p;
        n++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ',') { p++; continue; }
        if (*p == '}') break;
    }
    *out = e;
    return n;
}

/* 解析 style 对象（vstart..vend），紧凑重输出为一行 {"a": "b", "c": "d"} */
static void buf_emit_style_compact(Buf *out, SEntry *style_obj, int style_n) {
    buf_char(out, '{');
    for (int i = 0; i < style_n; i++) {
        if (i > 0) buf_str(out, ", ");
        buf_char(out, '"');
        buf_str(out, style_obj[i].key);
        buf_str(out, "\": ");
        buf_append(out, style_obj[i].vstart, (size_t)(style_obj[i].vend - style_obj[i].vstart));
    }
    buf_char(out, '}');
}

/* 重新生成规范化的 settings 对象（含 CRLF），写入 out。
 * raw 是原始 settings JSON（含 { }）。d 提供轨道信息以重映射 note_styles。 */
static void emit_settings(Buf *out, const char *raw, UstxData *d) {
    if (!raw || !*raw) {
        /* 空设置：仅补 selected_track_no */
        buf_str(out, "{\r\n    \"selected_track_no\": 0\r\n  }");
        return;
    }
    SEntry *entries = NULL;
    int n = split_object_entries(raw, &entries);

    /* 原文 note_styles（键=全局行号, 值=样式）。
       note_styles 的键是全局音符行号，取值区间为 [0, note_count)，据此动态分配，
       避免音符数超过 8192 的大工程发生栈溢出。 */
    int style_cap = d->note_count > 0 ? d->note_count : 1;
    int *orig_style = (int*)calloc((size_t)style_cap, sizeof(int));
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].key, "note_styles") == 0 && entries[i].type == 2) {
            SEntry *ns = NULL;
            int nn = split_object_entries(entries[i].vstart, &ns);
            for (int j = 0; j < nn; j++) {
                int k = atoi(ns[j].key);
                char v[32]; int vl = (int)(ns[j].vend - ns[j].vstart);
                if (vl >= 31) vl = 31;
                memcpy(v, ns[j].vstart, vl); v[vl] = '\0';
                if (k >= 0 && k < style_cap) orig_style[k] = atoi(v);
            }
            free(ns);
            break;
        }
    }

    /* 选中轨（目标格式默认按第 0 轨处理）；收集该轨音符的全局行号 */
    int sel = 0;
    int *sel_rows = (int*)malloc((size_t)style_cap * sizeof(int));
    int sel_n = 0;
    for (int i = 0; i < d->note_count; i++) {
        if (d->notes[i].track_no == sel && sel_n < style_cap) sel_rows[sel_n++] = i;
    }
    /* selected_track_no 非 0 时：构建过滤与 note_styles 均按选中轨 */
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].key, "selected_track_no") == 0) {
            /* 若显式存在则读取 */
            char v[32]; int vl = (int)(entries[i].vend - entries[i].vstart);
            if (vl >= 31) vl = 31;
            memcpy(v, entries[i].vstart, vl); v[vl] = '\0';
            int st = atoi(v);
            if (st != sel) {
                sel = st;
                sel_n = 0;
                for (int g = 0; g < d->note_count; g++)
                    if (d->notes[g].track_no == sel && sel_n < style_cap) sel_rows[sel_n++] = g;
            }
        }
    }

    buf_str(out, "{\r\n");
    for (int i = 0; i < n; i++) {
        SEntry *en = &entries[i];
        if (strcmp(en->key, "ustx_path") == 0) continue;
        if (strcmp(en->key, "note_styles") == 0) continue; /* 稍后重映射输出 */

        buf_str(out, "    \"");
        buf_str(out, en->key);
        buf_str(out, "\": ");

        if (strcmp(en->key, "styles") == 0 && en->type == 3) {
            /* 解析 styles 数组内的每个对象并紧凑输出 */
            const char *arr = en->vstart; /* '[' */
            buf_str(out, "[\r\n");
            const char *q = arr + 1;
            for (;;) {
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' || *q == ',') q++;
                if (*q == ']') break;
                if (*q == '{') {
                    SEntry *se = NULL;
                    int sn = split_object_entries(q, &se);
                    /* 定位该对象结束 */
                    const char *after = json_skip_value(q);
                    buf_str(out, "      ");
                    buf_emit_style_compact(out, se, sn);
                    q = after;
                    /* 若后面还有元素则加逗号 */
                    const char *r = q;
                    while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
                    if (*r == ',') {
                        buf_str(out, ",\r\n");
                        q = r + 1;
                    } else {
                        buf_str(out, "\r\n");
                        while (*q && *q != ']') q++;
                    }
                    free(se);
                } else q++;
            }
            buf_str(out, "    ],\r\n");
        } else {
            /* 标量/字符串/其他数组：原样复制值 */
            buf_append(out, en->vstart, (size_t)(en->vend - en->vstart));
            buf_str(out, ",\r\n");
        }
    }

    /* note_styles：原文取值，重映射到选中轨音符下标 */
    if (sel_n > 0) {
        buf_str(out, "    \"note_styles\": {\r\n");
        for (int i = 0; i < sel_n; i++) {
            int g = sel_rows[i];
            char kb[16]; snprintf(kb, sizeof(kb), "%d", i);
            buf_str(out, "      \"");
            buf_str(out, kb);
            buf_str(out, "\": ");
            buf_int(out, orig_style[g]);
            buf_str(out, i < sel_n - 1 ? ",\r\n" : "\r\n");
        }
        buf_str(out, "    },\r\n");
    }

    buf_str(out, "    \"selected_track_no\": 0\r\n");
    buf_str(out, "  }");

    free(entries);
    free(orig_style);
    free(sel_rows);
}

/* ===================== 目标格式 JSON 生成 ===================== */

static void generate_v3_json(Buf *out, UstxData *d, const char *settings_json) {
    buf_str(out, "{\r\n");
    buf_str(out, "  \"format\": \"ustxPlayer.uprj\",\r\n");
    buf_str(out, "  \"version\": 3,\r\n");
    buf_str(out, "  \"ustx_data\": {\r\n");
    buf_str(out, "    \"version\": ");
    buf_json_str(out, d->version);
    buf_str(out, ",\r\n");
    buf_str(out, "    \"tempo\": ");
    buf_dbl(out, d->tempo);
    buf_str(out, ",\r\n");
    buf_str(out, "    \"tempo_count\": ");
    buf_int(out, d->tempo_count);
    buf_str(out, ",\r\n");

    /* tempos（每元素一行） */
    buf_str(out, "    \"tempos\": [\r\n");
    for (int i = 0; i < d->tempo_count; i++) {
        buf_str(out, "      {\"position\": ");
        buf_int(out, d->tempos[i].position);
        buf_str(out, ", \"bpm\": ");
        buf_int(out, d->tempos[i].bpm);
        buf_str(out, "}");
        buf_str(out, i < d->tempo_count - 1 ? ",\r\n" : "\r\n");
    }
    buf_str(out, "    ],\r\n");

    buf_str(out, "    \"tracks\": ");
    buf_int(out, d->tracks);
    buf_str(out, ",\r\n");

    /* tracks_info（每元素一行） */
    buf_str(out, "    \"tracks_info\": [\r\n");
    for (int i = 0; i < d->tracks_info_count; i++) {
        buf_str(out, "      {\"track_no\": ");
        buf_int(out, d->tracks_info[i].track_no);
        buf_str(out, ", \"track_name\": ");
        buf_json_str(out, d->tracks_info[i].track_name);
        buf_str(out, ", \"note_count\": ");
        buf_int(out, d->tracks_info[i].note_count);
        buf_str(out, "}");
        buf_str(out, i < d->tracks_info_count - 1 ? ",\r\n" : "\r\n");
    }
    buf_str(out, "    ],\r\n");

    /* empty_tracks（默认 expand 格式，非紧凑） */
    buf_str(out, "    \"empty_tracks\": [\r\n");
    for (int i = 0; i < d->empty_tracks_count; i++) {
        buf_str(out, "      {\r\n");
        buf_str(out, "        \"track_no\": ");
        buf_int(out, d->empty_tracks[i].track_no);
        buf_str(out, ",\r\n");
        buf_str(out, "        \"track_name\": ");
        buf_json_str(out, d->empty_tracks[i].track_name);
        buf_str(out, ",\r\n");
        buf_str(out, "        \"note_count\": 0\r\n");
        buf_str(out, "      }");
        buf_str(out, i < d->empty_tracks_count - 1 ? ",\r\n" : "\r\n");
    }
    buf_str(out, "    ],\r\n");

    buf_str(out, "    \"wave_part_count\": ");
    buf_int(out, d->wave_part_count);
    buf_str(out, ",\r\n");

    /* notes */
    buf_str(out, "    \"notes\": [\r\n");
    for (int i = 0; i < d->note_count; i++) {
        Note *n = &d->notes[i];
        char index[8];
        snprintf(index, sizeof(index), "%04d", i);
        buf_str(out, "      {\"index\": ");
        buf_json_str(out, index);
        buf_str(out, ", \"position\": ");
        buf_int(out, n->position);
        buf_str(out, ", \"length\": ");
        buf_int(out, n->length);
        buf_str(out, ", \"lyric\": ");
        buf_json_str(out, n->lyric);
        buf_str(out, ", \"note_num\": ");
        buf_int(out, n->note_num);
        buf_str(out, ", \"track_no\": ");
        buf_int(out, n->track_no);
        buf_str(out, ", \"pitch_bend\": [");
        for (int j = 0; j < n->pitch_count; j++) {
            if (j > 0) buf_str(out, ", ");
            buf_int(out, n->pitch_bend[j]);
        }
        buf_str(out, "], \"pitch_ticks\": [");
        for (int j = 0; j < n->pitch_count; j++) {
            if (j > 0) buf_str(out, ", ");
            buf_int(out, n->pitch_ticks[j]);
        }
        buf_str(out, "]}");
        if (i < d->note_count - 1) buf_str(out, ",");
        buf_str(out, "\r\n");
    }
    buf_str(out, "    ]\r\n");
    buf_str(out, "  },\r\n");

    /* settings */
    buf_str(out, "  \"settings\": ");
    emit_settings(out, settings_json, d);
    buf_str(out, "\r\n}");
}

/* ===================== 输出路径生成 ===================== */

static char *make_output_path(const char *input) {
    const char *dot = strrchr(input, '.');
    size_t base_len = dot ? (size_t)(dot - input) : strlen(input);
    size_t cap = base_len + 32;
    char *out = (char*)malloc(cap);
    memcpy(out, input, base_len);
    memcpy(out + base_len, "_v3", 3); /* 前段：名字_v3 */
    out[base_len + 3] = '\0';
    /* 目标已存在则自动加序号：名字_v3.uprj → 名字_v3_1.uprj → 名字_v3_2.uprj ... */
    for (int k = 0; ; k++) {
        char *ap = out + base_len + 3;
        size_t rem = cap - (base_len + 3);
        int n = (k == 0) ? snprintf(ap, rem, ".uprj")
                         : snprintf(ap, rem, "_%d.uprj", k);
        if (n < 0 || (size_t)n >= rem) {
            cap *= 2;
            out = (char*)realloc(out, cap);
            continue;
        }
        if (!file_exists(out)) break;
    }
    return out;
}

/* ===================== 主函数 ===================== */

static void pause_exit(int code) {
    printf("\n按 Enter 退出...");
    /* 清空输入缓冲 */
    int c; while ((c = getchar()) != '\n' && c != EOF) {}
    getchar();
    exit(code);
}

static void print_usage(void) {
    printf("ustxPlayer 的工程格式转换工具（uPl：ustxPlayer 与 ustPlayer 共同项目）\n\n");
    printf("用法：拖放 .uplr 文件到此程序上即可自动转换\n\n");
    printf("在 ustxPlayer 与 ustPlayer 两个程序之间转换工程格式\n");
}

int main(int argc, char *argv[]) {
    const char *input_path = NULL;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    /* 用宽字符命令行拿取原始路径并转成 UTF-8，
       避免中文路径在 ANSI(GBK)/UTF-8 之间转换而乱码 */
    int ac = 0;
    LPWSTR *wav = CommandLineToArgvW(GetCommandLineW(), &ac);
    if (ac < 2) {
        print_usage();
        if (wav) LocalFree(wav);
        pause_exit(0);
    }
    static char u8path[4096];
    if (!wide_to_utf8(wav[1], u8path, (int)sizeof u8path)) {
        printf("错误：无法解析文件路径\n");
        if (wav) LocalFree(wav);
        pause_exit(1);
    }
    if (wav) LocalFree(wav);
    input_path = u8path;
#else
    if (argc < 2) { print_usage(); return 0; }
    input_path = argv[1];
#endif

    size_t file_len;
    char *json = read_file(input_path, &file_len);
    if (!json) {
        printf("错误：无法读取文件 %s\n", input_path);
        pause_exit(1);
    }

    /* 检查 format */
    const char *fmt_s, *fmt_e;
    if (!find_json_string(json, "format", &fmt_s, &fmt_e)) {
        printf("错误：无法找到 format 字段\n");
        free(json);
        pause_exit(1);
    }
    size_t fmt_len = fmt_e - fmt_s;
    if (fmt_len < 10 || strncmp(fmt_s, "ustxPlayer", 10) != 0) {
        printf("错误：不是 ustxPlayer 工程文件\n");
        free(json);
        pause_exit(1);
    }

    /* 检查 version */
    const char *ver_s, *ver_e;
    int version = 0;
    if (find_json_value(json, "version", &ver_s, &ver_e)) {
        char vbuf[16];
        int vlen = ver_e - ver_s;
        if (vlen >= 15) vlen = 15;
        memcpy(vbuf, ver_s, vlen);
        vbuf[vlen] = '\0';
        version = atoi(vbuf);
    }

    if (version == 3) {
        printf("文件已是目标格式，无需转换\n");
        free(json);
        pause_exit(0);
    }

    if (version != 2) {
        printf("错误：不支持的版本 %d\n", version);
        free(json);
        pause_exit(1);
    }

    /* 提取 ustx_content */
    char *ustx_content = extract_json_string(json, "ustx_content");
    if (!ustx_content || !*ustx_content) {
        printf("错误：无法提取 ustx_content（可能为空）\n");
        free(json);
        pause_exit(1);
    }

    /* 提取 settings（原始内容，交给 emit_settings 重写为规范结构） */
    char *settings_raw = extract_settings(json);

    /* 解析 USTX YAML */
    printf("正在解析 USTX 内容...\n");
    UstxData udata;
    udata_init(&udata);
    parse_ustx(ustx_content, &udata);

    printf("解析完成：%d 个音符，%d 条轨道，BPM=%.1f\n",
           udata.note_count, udata.tracks, udata.tempo);

    if (udata.note_count == 0) {
        printf("警告：未解析到任何音符\n");
    }

    /* 生成目标格式 JSON */
    Buf out;
    buf_init(&out);
    generate_v3_json(&out, &udata, settings_raw);

    /* 写入输出文件 */
    char *output_path = make_output_path(input_path);
    if (write_file(output_path, out.p, out.len) != 0) {
        printf("错误：无法写入 %s\n", output_path);
        free(output_path);
        free(out.p);
        free(json);
        free(ustx_content);
        free(settings_raw);
        pause_exit(1);
    }

    printf("\n转换成功\n");
    printf("输出文件：%s\n", output_path);
    printf("文件大小：%.1f KB\n", (double)out.len / 1024.0);

    /* 释放内存 */
    free(output_path);
    free(out.p);
    free(json);
    free(ustx_content);
    free(settings_raw);
    for (int i = 0; i < udata.note_count; i++) {
        free(udata.notes[i].pitch_bend);
        free(udata.notes[i].pitch_ticks);
    }
    free(udata.notes);
    free(udata.tempos);
    free(udata.tracks_info);
    free(udata.all_tracks);
    free(udata.empty_tracks);
    free(udata.wave_flag);

    pause_exit(0);
    return 0;
}
