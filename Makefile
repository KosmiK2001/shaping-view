CC     ?= gcc
PKG    := gtk+-3.0
CFLAGS ?= -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags $(PKG))
LDLIBS := $(shell pkg-config --libs $(PKG)) -lm

# трей: libappindicator (StatusNotifier) если есть, иначе GtkStatusIcon (XEmbed)
ifneq ($(shell pkg-config --exists appindicator3-0.1 && echo yes),)
CFLAGS  += -DHAVE_APPINDICATOR $(shell pkg-config --cflags appindicator3-0.1)
LDLIBS  += $(shell pkg-config --libs appindicator3-0.1)
endif

BIN     := shaping-view
BIN_DBG := shaping-view-debug
BIN_SAN := shaping-view-san
SRC     := shaping-view.c

# ---------------------------------------------------------------
# all / release — финальная сборка: -O2 -march=native, затем
#                 strip -s и upx -9 (upx пропускается, если не установлен)
# debug          — промежуточная разработка: -O0 -g3 + макросы + исходники
#                  в отладочной информации, без оптимизаций
# sanitize       — debug + ASan/UBSan (ловим порчи памяти на раннем этапе)
# ---------------------------------------------------------------

all: release

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -O2 -march=native -o $@ $(SRC) $(LDLIBS)

release: $(BIN)
	strip -s $(BIN)
	@if command -v upx >/dev/null 2>&1; then upx -9 -q $(BIN); \
	 else echo "upx не установлен — сжатие пропущено"; fi

debug: CFLAGS += -O0 -g3 -DDEBUG -fno-omit-frame-pointer
debug: $(BIN_DBG)

$(BIN_DBG): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

sanitize: CFLAGS += -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: $(BIN_SAN)

$(BIN_SAN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

run-debug: debug
	./$(BIN_DBG)

clean:
	rm -f $(BIN) $(BIN_DBG) $(BIN_SAN)

.PHONY: all debug sanitize release run run-debug clean
