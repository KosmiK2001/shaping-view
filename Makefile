CC     ?= gcc
PKG    := gtk+-3.0
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags $(PKG))
LDLIBS := $(shell pkg-config --libs $(PKG)) -lm

# трей: libappindicator (StatusNotifier) если есть, иначе GtkStatusIcon (XEmbed)
ifneq ($(shell pkg-config --exists appindicator3-0.1 && echo yes),)
CFLAGS  += -DHAVE_APPINDICATOR $(shell pkg-config --cflags appindicator3-0.1)
LDLIBS  += $(shell pkg-config --libs appindicator3-0.1)
endif

BIN := shaping-view
SRC := shaping-view.c

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
