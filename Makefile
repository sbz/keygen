CC = cc
SECURE_CFLAGS = -fsanitize=address -fsantizie=leak
XFT_CFLAGS = $(shell pkg-config --cflags xft)
XFT_LDFLAGS = $(shell pkg-config --libs xft)
CFLAGS = -Wall -Wextra -O2 $(XFT_CFLAGS)
LDFLAGS = -lX11 -lmpg123 -lasound -lpthread -lm -ljpeg $(XFT_LDFLAGS)

TARGET = keygen
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

debug: $(OBJ)
	$(CC) $(CFLAGS) $(SECURE_FLAGS) -o $(TARGET) $^ $(LDFLAGS)


clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
