#include "shell.h"
#include "terminal.h"
#include "exec_test.h"
#include "ram_exec.h"
#include "bapx.h"
#include "../libct/fsc/fs-control.h"
#include "../libc/memmgr.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#define CT_PATH_MAX 128

// 起動ドライブは内蔵ドライブ固定(SDカード等への切り替えは別コマンドで拡張する想定)
static const char *root_drive = "\\\\drv0\\";
static char current_path[CT_PATH_MAX];

void ct_shell_init(void)
{
	strncpy(current_path, root_drive, CT_PATH_MAX - 1);
	current_path[CT_PATH_MAX - 1] = '\0';
}

// 空白区切りでコマンド名と引数1つに分割する(引数はスペースを含まない前提の簡易版)
static void split_command(const char *line, char *cmd, size_t cmd_size, char *arg, size_t arg_size)
{
	size_t i = 0;
	while (line[i] != '\0' && line[i] != ' ' && i + 1 < cmd_size) {
		cmd[i] = line[i];
		++i;
	}
	cmd[i] = '\0';

	while (line[i] == ' ')
		++i;

	size_t j = 0;
	while (line[i] != '\0' && j + 1 < arg_size) {
		arg[j] = line[i];
		++i;
		++j;
	}
	arg[j] = '\0';
}

static int path_is_absolute(const char *path)
{
	return path[0] == '\\';
}

// current_path を基準に、相対/絶対どちらの指定でも移動先パスを組み立てる
static void resolve_path(const char *arg, char *out, size_t out_size)
{
	if (arg[0] == '\0' || strcmp(arg, ".") == 0) {
		strncpy(out, current_path, out_size - 1);
		out[out_size - 1] = '\0';
		return;
	}
	if (strcmp(arg, "..") == 0) {
		strncpy(out, current_path, out_size - 1);
		out[out_size - 1] = '\0';
		size_t len = strlen(out);
		// 末尾の "\" を落としてから、その前の "\" までを親ディレクトリとみなす
		if (len > 0 && out[len - 1] == '\\')
			out[--len] = '\0';
		char *sep = strrchr(out, '\\');
		if (sep != NULL)
			sep[1] = '\0';
		return;
	}
	if (path_is_absolute(arg)) {
		strncpy(out, arg, out_size - 1);
		out[out_size - 1] = '\0';
	} else {
		strncpy(out, current_path, out_size - 1);
		out[out_size - 1] = '\0';
		strncat(out, arg, out_size - strlen(out) - 1);
	}
	size_t len = strlen(out);
	if (len == 0 || out[len - 1] != '\\') {
		if (len + 1 < out_size) {
			out[len] = '\\';
			out[len + 1] = '\0';
		}
	}
}

static void cmd_pwd(void)
{
	ct_terminal_puts(current_path);
	ct_terminal_putc('\n');
}

static void cmd_cd(const char *arg)
{
	char target[CT_PATH_MAX];
	resolve_path(arg, target, sizeof(target));

	struct file_list_result result = get_file_list(target);
	if (result.success != 0) {
		ct_terminal_puts("cd: no such directory\n");
		return;
	}
	if (result.entries != NULL)
		memmgr_free(result.entries);

	strncpy(current_path, target, CT_PATH_MAX - 1);
	current_path[CT_PATH_MAX - 1] = '\0';
}

// sys_findfirst/next が返す type の値(fs-control.c の呼び出し元実績に基づく)
#define FS_TYPE_FILE 1
#define FS_TYPE_DIR  5

static void cmd_ls(void)
{
	struct file_list_result result = get_file_list(current_path);
	if (result.success != 0) {
		ct_terminal_puts("ls: cannot access path\n");
		return;
	}

	char line[CT_TERM_COLS + 1];
	for (int i = 0; i < result.count; ++i) {
		const char *tag = "[?]   ";
		if (result.entries[i].type == FS_TYPE_DIR)
			tag = "[DIR] ";
		else if (result.entries[i].type == FS_TYPE_FILE)
			tag = "[FILE]";

		strncpy(line, tag, sizeof(line) - 1);
		line[sizeof(line) - 1] = '\0';
		strncat(line, " ", sizeof(line) - strlen(line) - 1);
		strncat(line, result.entries[i].name, sizeof(line) - strlen(line) - 1);

		ct_terminal_puts(line);
		ct_terminal_putc('\n');
	}
	if (result.count == 0)
		ct_terminal_puts("(empty)\n");

	if (result.entries != NULL)
		memmgr_free(result.entries);
}

// BAPX動画を再生する。引数はファイル名(カレントディレクトリ基準)か絶対パス。
static void cmd_play(const char *arg)
{
	if (arg[0] == '\0') {
		ct_terminal_puts("usage: play <file.bin>\n");
		return;
	}

	char target[CT_PATH_MAX];
	if (path_is_absolute(arg)) {
		strncpy(target, arg, sizeof(target) - 1);
		target[sizeof(target) - 1] = '\0';
	} else {
		// resolve_path は ls 用に末尾へ "*" を付けるため、ここでは使わない
		strncpy(target, current_path, sizeof(target) - 1);
		target[sizeof(target) - 1] = '\0';
		size_t len = strlen(target);
		if (len > 0 && target[len - 1] != '\\')
			strncat(target, "\\", sizeof(target) - strlen(target) - 1);
		strncat(target, arg, sizeof(target) - strlen(target) - 1);
	}

	int rc = bapx_play_file(target);

	// 再生でVRAMを潰しているので端末表示を描き直す
	ct_terminal_draw();
	if (rc != 0) {
		ct_terminal_puts("play: ");
		ct_terminal_puts(bapx_strerror(rc));
		ct_terminal_putc('\n');
	}
}

void ct_shell_exec(const char *line)
{
	char cmd[16];
	char arg[CT_PATH_MAX];
	split_command(line, cmd, sizeof(cmd), arg, sizeof(arg));

	if (cmd[0] == '\0')
		return;

	if (strcasecmp(cmd, "PWD") == 0) {
		cmd_pwd();
	} else if (strcasecmp(cmd, "CD") == 0) {
		cmd_cd(arg);
	} else if (strcasecmp(cmd, "LS") == 0) {
		cmd_ls();
	} else if (strcasecmp(cmd, "PLAY") == 0) {
		cmd_play(arg);
	} else if (strcasecmp(cmd, "CHECKER") == 0) {
		// 切り分け用: デコーダを使わず市松模様を直接backbufへ書いて
		// DMA転送するだけ。これでも崩れるならDMA/backbuf/キャッシュ側、
		// 崩れないならBAPXデコーダ側が原因と判断できる。
		int n = arg[0] != '\0' ? atoi(arg) : 1;
		bapx_test_checker(n);
		ct_terminal_draw();
	} else if (strcasecmp(cmd, "CHECKERIO") == 0) {
		// 切り分け用2: playと同じファイルI/O(seek/read)を毎フレーム
		// 通しつつ、読んだ内容は捨てて市松模様を描く。引数はplayと同じ
		// ファイル名(フレーム数は30固定)。これでもズレるならファイルI/O
		// 側、ズレないならランレングス展開ロジック側が原因と判断できる。
		if (arg[0] == '\0') {
			ct_terminal_puts("usage: checkerio <file.bin>\n");
		} else {
			char target[CT_PATH_MAX];
			if (path_is_absolute(arg)) {
				strncpy(target, arg, sizeof(target) - 1);
				target[sizeof(target) - 1] = '\0';
			} else {
				strncpy(target, current_path, sizeof(target) - 1);
				target[sizeof(target) - 1] = '\0';
				size_t len = strlen(target);
				if (len > 0 && target[len - 1] != '\\')
					strncat(target, "\\", sizeof(target) - strlen(target) - 1);
				strncat(target, arg, sizeof(target) - strlen(target) - 1);
			}
			bapx_test_checker_with_io(target, 30);
		}
		ct_terminal_draw();
	} else if (strcasecmp(cmd, "CLRVRAM") == 0) {
		// 実験用: BSS上の機械語を実行できるか検証するコマンド
		ct_terminal_puts("executing RAM code...\n");
		exec_test_clear_vram();
	} else if (strcasecmp(cmd, "RAMTEST") == 0) {
		// 実験用: .ram_textセクション(ROM->RAM再配置)の複数関数呼び出しを検証。
		// 起動時には行わず、このコマンドで初めて0x8c800000へコピーする
		// (未検証のアドレスなので、使わない限り触れないようにする)。
		static int ram_ready = 0;
		if (!ram_ready) {
			ram_exec_init();
			ram_ready = 1;
		}
		int n = arg[0] != '\0' ? atoi(arg) : 5;
		int result = ram_exec_test(n);
		char line[64];
		strncpy(line, "ram_exec_test(", sizeof(line) - 1);
		line[sizeof(line) - 1] = '\0';
		char numbuf[16];
		sprintf(numbuf, "%d", n);
		strncat(line, numbuf, sizeof(line) - strlen(line) - 1);
		strncat(line, ") = ", sizeof(line) - strlen(line) - 1);
		sprintf(numbuf, "%d", result);
		strncat(line, numbuf, sizeof(line) - strlen(line) - 1);
		strncat(line, " (expect same as n)\n", sizeof(line) - strlen(line) - 1);
		ct_terminal_puts(line);
	} else {
		ct_terminal_puts(cmd);
		ct_terminal_puts(": command not found\n");
	}
}
