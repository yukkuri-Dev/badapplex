#ifndef FAKETERMINAL_EXEC_TEST_H
#define FAKETERMINAL_EXEC_TEST_H

// 実験用: BSS上に置いた生の機械語バイト列を関数として実行できるか検証する。
// SH4はXIP(ROM上のコードを直接フェッチする)方式だが、RAM上に書いたコードを
// 実行できればLuaのコードサイズ問題を回避できる可能性があるための検証。
void exec_test_clear_vram(void);

#endif /* FAKETERMINAL_EXEC_TEST_H */
