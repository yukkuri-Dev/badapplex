#ifndef FAKETERMINAL_RAM_EXEC_H
#define FAKETERMINAL_RAM_EXEC_H

// 実験用: リンカスクリプト(exword.ld)の.ram_textセクションに配置された
// 複数関数(内部で呼び合う)を、起動時にROMからRAM(0x8c800000)へコピーして
// から呼び出せるか検証する。CLRVRAM(exec_test.c)は単一バイト列の直接実行
// だったが、こちらは通常のC関数呼び出し規約・複数関数間の呼び出しが
// RAM実行後も正しく機能するかを見るためのテスト。
void ram_exec_init(void);
int ram_exec_test(int n);

#endif /* FAKETERMINAL_RAM_EXEC_H */
