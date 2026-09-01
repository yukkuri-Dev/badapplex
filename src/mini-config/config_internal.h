// config_internal.h
#ifndef CONFIG_INTERNAL_H
#define CONFIG_INTERNAL_H
#include "config.h"      // ★これが要る

// key/value は常に strdup() で確保する(固定長バッファへの直接書き込みは禁止)
struct config_entry {
    char *key;
    char *value;
};

struct config_t {
    char *drive_path;
    int loaded;
    struct config_entry *entries;  // エントリ配列(libconfigのsettingツリーに相当する簡易版)
    int entry_count;
    int entry_capacity;
};
#endif