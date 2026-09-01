#ifndef FAKETERMINAL_SHELL_H
#define FAKETERMINAL_SHELL_H

// カレントパスを初期状態(内蔵ドライブのルート)にリセットする
void ct_shell_init(void);
// 確定した1行(コマンドライン)を解釈して実行し、結果をターミナルに出力する
void ct_shell_exec(const char *line);

#endif /* FAKETERMINAL_SHELL_H */
