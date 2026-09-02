.SUFFIXES:

ifeq ($(strip $(DEVKITSH4)),)
$(error "Please set DEVKITSH4 in your environment. export DEVKITSH4=<path to sdk>")
endif
include $(DEVKITSH4)/exword_rules

TARGET       := apple
MODNAME      := helloapple
APPTITLE     := Hello, APPLE!
APPID        := APPLE
APPMOD       := $(TARGET).d01

SOURCEDIR    := src
HTMLDIR      := html
INSTALLDIR   := $(HOME)/.local/share/exword
BUILDS       := ja cn
EXCLUDE      :=
SRCDIRS      := $(SOURCEDIR) $(SOURCEDIR)/libc $(SOURCEDIR)/libct $(SOURCEDIR)/libct/fsc $(SOURCEDIR)/faketerminal
CFILES       := $(filter-out $(EXCLUDE),$(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.c)))
SFILES       := $(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.s))
OBJECTS      := $(CFILES:.c=.o) $(SFILES:.s=.o)

CC_OPTS      :=
# -mexword は暗黙で "-T exword.ld" を追加するため、-T を渡すだけでは
# 置き換わらず標準スクリプトと二重に読まれてしまう(_sbss/_ebss 等の
# シンボルが壊れ、実機で起動直後にクラッシュする実害があった)。
# -specs=no_exword_ld.specs でその自動付加を止め、exword_ram.ld 側の
# STARTUP("exword_crt0.o") で crt0 を明示的にリンクする。
LDFLAGS      := -Wall -std=gnu17 -nostdlib -specs=no_exword_ld.specs -T exword_ram.ld -L$(DEVKITPRO)/libdataplus/lib -ldataplus -lgraphics -lsh4a -lgcc
CFLAGS       := -Wall -std=gnu17 -fno-builtin -I$(DEVKITPRO)/libdataplus/include -I$(SOURCEDIR) -I$(SOURCEDIR)/libc/include -O3 $(CC_OPTS)
ASFLAGS      := -Wall -std=gnu17 -m4-nofpu

# ocbwb/icbi (キャッシュ操作命令) は -m3 ではアセンブルできないため、
# これらを使うファイルだけ SH4 系の命令セットでコンパイルする。
# MACHDEP の後ろに CFLAGS が並ぶので、後勝ちで -m3 を上書きできる。
src/faketerminal/exec_test.o: CFLAGS += -m4-nofpu
src/faketerminal/ram_exec.o:  CFLAGS += -m4-nofpu
src/faketerminal/bapx.o:      CFLAGS += -m4-nofpu
src/faketerminal/bapx_dma.o:  CFLAGS += -m4-nofpu

# 切り分け用: SDL2ハーネス(同一ロジック、host gcc -O3)ではフレームズレが
# 再現しないため、-O3最適化(sh-elf-gcc)が原因かどうかを確認している。
# CFLAGSは元々-O3を含むが、-O0を後ろに追加すると後勝ちでこちらが有効になる。
src/faketerminal/bapx.o:      CFLAGS += -O0

# 切り分け用: lcdc_copy_vram()のDMA完了待ちの後にウェイトを挟んでも
# ズレは変わらなかった(無罪と判定済み)ので今は外している。
# src/faketerminal/bapx_dma.o:  CFLAGS += -DBAPX_DMA_POST_COPY_DELAY_TICKS=1000

# 切り分け用: SAR3にVRAM(0xac200000)以外の任意アドレスを指定すること
# 自体がこのハードウェアで想定されていないのではという仮説の検証。
# backbufをmemmgr poolではなくVRAM本体(528*320*2=0x52800バイト)の
# 直後、32バイト境界に切り上げたアドレスへ固定配置する。
src/faketerminal/bapx_dma.o:  CFLAGS += -DBAPX_DMA_FIXED_BACKBUF=0xac252800

app: $(addprefix build/,$(addsuffix /$(APPID),$(BUILDS)))

.SECONDEXPANSION:
build/%/$(APPID): $(TARGET).d01 $$(wildcard $(HTMLDIR)/$$*/*.htm)
	@echo building $* version in $@...
	@mkdir -p $@
	@cp $(TARGET).d01 $@
	@for f in $(HTMLDIR)/$*/*.htm; do \
		sed -e 's/@APPTITLE/$(APPTITLE)/g' -e 's/@APPID/$(APPID)/g' -e 's/@APPMOD/$(APPMOD)/g' $$f > $@/$$(basename $$f); \
	done
	@touch $@/fileinfo.cji

$(TARGET).elf: $(OBJECTS)

install: app
	@echo installing to $(INSTALLDIR)...
	@mkdir -p $(INSTALLDIR)
	@cp -r build/* $(INSTALLDIR)/
	@echo 'You can now install this app to EX-word by `dict install $(APPID)` in libexword.'

clean:
	@echo clean $(OBJECTS) $(TARGET).elf $(TARGET).elf.map $(TARGET).d01
	@rm -fr build $(OBJECTS) $(TARGET).elf $(TARGET).elf.map $(TARGET).d01
