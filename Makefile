CC = gcc
CFLAGS = -Wall -Wextra -O2 `pkg-config --cflags xft`
LDFLAGS = -lX11 -lmpg123 -lasound -lpthread -lm -ljpeg `pkg-config --libs xft`

TARGET = keygen
SRC = keygen.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
