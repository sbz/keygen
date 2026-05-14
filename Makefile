CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lmpg123 -lasound -lpthread -lm -ljpeg

TARGET = keygen
SRC = keygen.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
