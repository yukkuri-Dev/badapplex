#include "libc/memmgr.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscalls/syscalls.h>
#include "ex-libc/snprint.h"
#include "libct/fsc/fs-control.h"
#include "config_internal.h"

#define DIR 5
#define FILE_RD   0x1
#define FILE_WR   0x2
#define FILE_RDWR 0x3

#define CONFIG_KEY_MAXLEN 64
#define CONFIG_VAL_MAXLEN 256
#define CONFIG_FILE_MAXLEN (1024 * 8)

static int is_config_exist(config_t *conf) {
    struct file_list_result files = get_file_list(conf->drive_path);
    int found = -1;
    for (int i = 0; i < files.count; i++) {
        if (files.entries[i].type == DIR) {
            if (strcmp(files.entries[i].name, "config") == 0) {
                found = 0;
                break;
            }
        }
    }
    free(files.entries);
    return found;
}

static void config_settings_path(config_t *conf, char *path, size_t path_size) {
    snprintf(path, path_size, "%s\\config\\settings.conf", conf->drive_path);
}

static void config_free_entries(config_t *conf) {
    for (int i = 0; i < conf->entry_count; i++) {
        free(conf->entries[i].key);
        free(conf->entries[i].value);
    }
    conf->entry_count = 0;
}

static struct config_entry *config_find_entry(config_t *conf, const char *key) {
    for (int i = 0; i < conf->entry_count; i++) {
        if (strcmp(conf->entries[i].key, key) == 0) {
            return &conf->entries[i];
        }
    }
    return NULL;
}

static int config_ensure_capacity(config_t *conf) {
    if (conf->entry_count < conf->entry_capacity) return 0;

    int new_cap = conf->entry_capacity == 0 ? 8 : conf->entry_capacity * 2;
    struct config_entry *new_entries = memmgr_realloc(conf->entries, sizeof(struct config_entry) * new_cap);
    if (!new_entries) return -1;
    conf->entries = new_entries;
    conf->entry_capacity = new_cap;
    return 0;
}

// key/value 文字列を strdup して1エントリ追加する(パース時専用、重複キーは考慮しない)
static int config_add_entry(config_t *conf, const char *key, const char *value) {
    if (config_ensure_capacity(conf) < 0) return -1;

    char *key_dup = strdup(key);
    char *value_dup = strdup(value);
    if (!key_dup || !value_dup) {
        free(key_dup);
        free(value_dup);
        return -1;
    }

    conf->entries[conf->entry_count].key = key_dup;
    conf->entries[conf->entry_count].value = value_dup;
    conf->entry_count++;
    return 0;
}

// "key=value\n" 形式の1行を解析して追加する
static void config_parse_line(config_t *conf, char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;

    *eq = '\0';
    char *key = line;
    char *value = eq + 1;

    // 改行・復帰を除去
    char *nl = strpbrk(value, "\r\n");
    if (nl) *nl = '\0';

    if (key[0] == '\0') return;

    char keybuf[CONFIG_KEY_MAXLEN];
    char valbuf[CONFIG_VAL_MAXLEN];
    strncpy(keybuf, key, sizeof(keybuf) - 1);
    keybuf[sizeof(keybuf) - 1] = '\0';
    strncpy(valbuf, value, sizeof(valbuf) - 1);
    valbuf[sizeof(valbuf) - 1] = '\0';

    config_add_entry(conf, keybuf, valbuf);
}

int config_read_file(config_t *conf) {
    if (!conf) return -1;

    char path[256];
    config_settings_path(conf, path, sizeof(path));

    int fd = sys_open(path, FILE_RD);
    if (fd < 0) return -1;

    int filesize = sys_get_filesize(fd);
    if (filesize <= 0 || filesize > CONFIG_FILE_MAXLEN) {
        sys_close(fd);
        return -1;
    }

    char *buf = memmgr_alloc(filesize + 1);
    if (!buf) {
        sys_close(fd);
        return -1;
    }

    int total_read = 0;
    while (total_read < filesize) {
        int r = sys_read(fd, buf + total_read, filesize - total_read);
        if (r <= 0) break;
        total_read += r;
    }
    sys_close(fd);
    buf[total_read] = '\0';

    config_free_entries(conf);

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        config_parse_line(conf, line);
        line = next ? next + 1 : NULL;
    }

    free(buf);
    return 0;
}

config_t *config_init(const char *drive_path) {
    config_t *conf = malloc(sizeof(config_t));
    if (!conf) return NULL;

    conf->drive_path = strdup(drive_path);
    conf->loaded = 0;
    conf->entries = NULL;
    conf->entry_count = 0;
    conf->entry_capacity = 0;

    int ret = is_config_exist(conf);
    if (ret == -1) {
        ret = directory_create((char *)drive_path, "config");
        if (ret < 0) {
            free(conf->drive_path);
            free(conf);
            return NULL;
        }
    } else {
        // 既存の設定ファイルがあれば読み込む(無ければ空のまま続行)
        config_read_file(conf);
    }

    conf->loaded = 1;
    return conf;
}

void config_destroy(config_t *conf) {
    if (!conf) return;
    config_free_entries(conf);
    free(conf->entries);
    free(conf->drive_path);
    free(conf);
}

void config_close(config_t *conf) {
    config_destroy(conf);
}

int config_set_string(config_t *conf, const char *key, const char *value) {
    if (!conf || !key || !value) return -1;

    struct config_entry *entry = config_find_entry(conf, key);
    if (entry) {
        char *value_dup = strdup(value);
        if (!value_dup) return -1;
        free(entry->value);
        entry->value = value_dup;
        return 0;
    }

    return config_add_entry(conf, key, value);
}

int config_set_int(config_t *conf, const char *key, int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return config_set_string(conf, key, buf);
}

int config_set_bool(config_t *conf, const char *key, int value) {
    return config_set_string(conf, key, value ? "true" : "false");
}

int config_lookup_string(config_t *conf, const char *key, const char **out_value) {
    if (!conf || !key || !out_value) return CONFIG_FALSE;

    struct config_entry *entry = config_find_entry(conf, key);
    if (!entry) return CONFIG_FALSE;

    *out_value = entry->value;
    return CONFIG_TRUE;
}

int config_lookup_int(config_t *conf, const char *key, int *out_value) {
    const char *str;
    if (!config_lookup_string(conf, key, &str)) return CONFIG_FALSE;
    if (!out_value) return CONFIG_FALSE;

    *out_value = atoi(str);
    return CONFIG_TRUE;
}

int config_lookup_bool(config_t *conf, const char *key, int *out_value) {
    const char *str;
    if (!config_lookup_string(conf, key, &str)) return CONFIG_FALSE;
    if (!out_value) return CONFIG_FALSE;

    *out_value = (strcmp(str, "true") == 0 || strcmp(str, "1") == 0);
    return CONFIG_TRUE;
}

int config_remove(config_t *conf, const char *key) {
    if (!conf || !key) return -1;

    for (int i = 0; i < conf->entry_count; i++) {
        if (strcmp(conf->entries[i].key, key) == 0) {
            free(conf->entries[i].key);
            free(conf->entries[i].value);
            for (int j = i; j < conf->entry_count - 1; j++) {
                conf->entries[j] = conf->entries[j + 1];
            }
            conf->entry_count--;
            return 0;
        }
    }
    return -1;
}

int config_write_file(config_t *conf) {
    if (!conf) return -1;

    char path[256];
    config_settings_path(conf, path, sizeof(path));

    int fd = sys_open(path, FILE_WR);
    if (fd < 0) return -1;

    for (int i = 0; i < conf->entry_count; i++) {
        char line[CONFIG_KEY_MAXLEN + CONFIG_VAL_MAXLEN + 4];
        int len = snprintf(line, sizeof(line), "%s=%s\n",
                            conf->entries[i].key, conf->entries[i].value);
        if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
        sys_write(fd, line, len);
    }

    sys_close(fd);
    return 0;
}
