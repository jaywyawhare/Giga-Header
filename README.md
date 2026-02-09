# Giga-Header

Converts C projects into header-only files. Available as a CLI tool and a web interface.

## Features

- Clones a git repo, scans for `.c` and `.h` files
- Categorizes `#include` directives into standard, external, and project-local
- Inlines project-local headers recursively at point of use
- Deduplicates standard and external includes at the top of the output
- External library dependencies are preserved so the output still compiles

## Requirements

- GCC
- libjson-c-dev
- git

## Build

```bash
make install-deps   # Ubuntu/Debian
make
```

## Usage

### CLI

```bash
./server <git_url>
./server <git_url> -o output.h
```

### Web

```bash
./server serve
```

Opens on http://localhost:8080.

## Output Format

```c
#ifndef REPO_COMBINED_H
#define REPO_COMBINED_H

#include <stdio.h>
#include <stdlib.h>

#include <openssl/ssl.h>
#include <zlib.h>

// inlined project source code...

#endif
```

## License

This project is licensed under the **DBaJ-GPL** see the [LICENCE](LICENCE) file for the full terms.
