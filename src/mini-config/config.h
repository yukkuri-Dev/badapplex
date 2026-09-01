// config.h (公開ヘッダ)
// libconfig (https://hyperrealm.github.io/libconfig/) のAPIに寄せた
// 組み込み向けミニ設定ライブラリ

#ifndef MINI_CONFIG_H
#define MINI_CONFIG_H

#define CONFIG_TRUE  1
#define CONFIG_FALSE 0

typedef struct config_t config_t;

// 生成/破棄
// drive_path 配下に config ディレクトリが無ければ作成し、
// settings.conf が既存であればパースして読み込む(libconfigのconfig_init+config_read_fileに相当)
config_t *config_init(const char *drive_path);
void config_destroy(config_t *conf);
// 後方互換のため残す(config_destroy と同じ)
void config_close(config_t *conf);

// ファイル入出力
// 指定パスの settings.conf を読み込み、既存エントリを破棄して置き換える
int config_read_file(config_t *conf);
// 現在のエントリを settings.conf に書き出す
int config_write_file(config_t *conf);

// 値の設定(キーが無ければ新規作成、あれば上書き)
int config_set_string(config_t *conf, const char *key, const char *value);
int config_set_int(config_t *conf, const char *key, int value);
int config_set_bool(config_t *conf, const char *key, int value);

// 値の参照(libconfigのconfig_lookup_*と同じ規約: 成功でCONFIG_TRUE、失敗でCONFIG_FALSE)
int config_lookup_string(config_t *conf, const char *key, const char **out_value);
int config_lookup_int(config_t *conf, const char *key, int *out_value);
int config_lookup_bool(config_t *conf, const char *key, int *out_value);

// キーの削除
int config_remove(config_t *conf, const char *key);

#endif // MINI_CONFIG_H
