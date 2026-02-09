CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -Wno-format-truncation -Wno-stringop-truncation
LIBS = -ljson-c
TARGET = server
SOURCE = server.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	rm -f $(TARGET)

install-deps:
	sudo apt-get update
	sudo apt-get install -y libjson-c-dev git

run: $(TARGET)
	./$(TARGET) serve

check-libs:
	@pkg-config --exists json-c || (echo "json-c not found. Run 'make install-deps'" && exit 1)
	@which git > /dev/null 2>&1 || (echo "git not found. Install git." && exit 1)
	@echo "All required libraries are available!"

.PHONY: all clean install-deps run check-libs
