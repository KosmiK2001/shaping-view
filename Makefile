CC     ?= gcc
PKG    := gtk+-3.0
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS += $(shell pkg-config --cflags $(PKG))
LDLIBS := $(shell pkg-config --libs $(PKG)) -lm

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
