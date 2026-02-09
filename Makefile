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
	@if command -v pacman > /dev/null 2>&1; then \
		sudo pacman -S --needed json-c git; \
	elif command -v apt-get > /dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y libjson-c-dev git; \
	elif command -v dnf > /dev/null 2>&1; then \
		sudo dnf install -y json-c-devel git; \
	elif command -v brew > /dev/null 2>&1; then \
		brew install json-c git; \
	elif command -v choco > /dev/null 2>&1; then \
		echo "Windows detected (Chocolatey)."; \
		echo "For native builds, use MSYS2 (https://www.msys2.org) and run:"; \
		echo "  pacman -S --needed mingw-w64-x86_64-json-c git"; \
		exit 1; \
	elif [ "$$OS" = "Windows_NT" ]; then \
		echo "Windows detected but no supported package manager found."; \
		echo "Install MSYS2 (https://www.msys2.org) and run:"; \
		echo "  pacman -S --needed mingw-w64-x86_64-json-c git"; \
		exit 1; \
	else \
		echo "Unsupported package manager. Install json-c and git manually."; \
		exit 1; \
	fi

run: $(TARGET)
	./$(TARGET) serve

check-libs:
	@pkg-config --exists json-c || (echo "json-c not found. Run 'make install-deps'" && exit 1)
	@which git > /dev/null 2>&1 || (echo "git not found. Install git." && exit 1)
	@echo "All required libraries are available!"

.PHONY: all clean install-deps run check-libs
