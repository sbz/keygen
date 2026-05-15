CC = cc
CFLAGS = -Wall -Wextra -O2 `pkg-config --cflags xft`
LDFLAGS = -lX11 -lmpg123 -lasound -lpthread -lm -ljpeg `pkg-config --libs xft`

TARGET = keygen
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
