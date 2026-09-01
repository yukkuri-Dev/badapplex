/* devkitSH4 標準の exword.ld をベースに、ROM に置いたコードを起動時 RAM へ
 * コピーして実行するための .ram_text セクションを追加したもの。
 *
 * .ram_text は VMA (実行アドレス) 0x8c800000、LMA (実体) は ROM。
 * ram_exec_init() が __sramtext_load から __sramtext へ
 * __eramtext - __sramtext バイトをコピーする。セクション内の関数は
 * 0x8c800000 台に再配置されるため、コピー後は通常の C 呼び出しで
 * RAM 上のコードが動作する。ram_exec_init() は起動時には自動で呼ばれず、
 * RAMTEST コマンドを使ったときに一度だけ呼ばれる(未検証のアドレスなので
 * 使わない限り触れないようにしている)。
 *
 * シンボル名の先頭が 2 本のアンダースコアなのは -mrenesas ABI で
 * C シンボルに _ が 1 つ前置されるため (C 側の _sramtext = __sramtext)。
 *
 * 重要: -mexword は常に "-T exword.ld" を自動付加する。独自の -T を渡しても
 * 追加されるだけで置き換わらないため、何もしないと標準 exword.ld とこの
 * ファイルの両方が読まれて SECTIONS がマージされ、_sbss/_ebss 等のシンボルが
 * 壊れる(実機で起動直後にクラッシュする実害があった)。そのため Makefile 側で
 * -specs=no_exword_ld.specs を渡して自動付加そのものを止めている。
 * その結果 crt0 (exword_crt0.o) も自動では読まれなくなるので、
 * ここで明示的に STARTUP する必要がある。
 */
ENTRY("_start")
STARTUP("exword_crt0.o")

MEMORY
{
  rom (rx) : org = 0x70000080, len = 0x1FF80
  ram (w)  : org = 0x7400004c, len = 0x7FB4
}

PHDRS
{
  text    PT_LOAD FLAGS(5);  /* R E */
  data    PT_LOAD FLAGS(6);  /* RW  */
  bss     PT_LOAD FLAGS(6);  /* RW  */
}

SECTIONS
{
  .text :
  {
    _stext = .;
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(32);
    _etext = .;
  } > rom :text

  .bss :
  {
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > ram :bss

  .data :
  {
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(32);
    _edata = .;
  } > ram AT>rom :data

  .ram_text 0x8c800000 : AT (LOADADDR(.data) + SIZEOF(.data))
  {
    __sramtext = .;
    *(.ram_text)
    *(.ram_text*)
    . = ALIGN(32);
    __eramtext = .;
  } :data
  __sramtext_load = LOADADDR(.ram_text);

  /DISCARD/ :
  {
    *(.comment)
    *(.eh_frame)
  }
}
