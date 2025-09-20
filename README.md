
# tinydb – a minimal, single‑file SQLite‑style storage engine  

**tinydb** is a tiny C++‑17 library that demonstrates the low‑level building blocks of a SQLite‑like database.  
It implements a **page‑based storage manager**, packed on‑disk structures, and a tiny test driver (`main`) that opens (or creates) a database file, allocates pages, and persists the page count across runs.

> **Why this repo?**  
> It provides a clean, self‑contained starting point for anyone who wants to implement their own B‑Tree, SQL parser, or full‑featured embedded DB on top of a simple, well‑documented storage layer.

---

## Table of Contents
- [Features](#features)
- [Prerequisites](#prerequisites)
- [Building & Running](#building--running)
- [What the program does (demo)](#what-the-program-does-demo)
- [Design Overview](#design-overview)
  - [On‑disk page layout](#on-disk-page-layout)
  - [`StorageManager` API](#storagemanager-api)
  - [Data structures](#data-structures)
- [Extending tinydb](#extending-tinydb)
- [Testing & Validation](#testing--validation)
- [License](#license)
- [Contributing](#contributing)

---

## Features
- **Page‑oriented storage** – each page is exactly `PAGE_SIZE` (4096 bytes).  
- **Packed structs** (`#pragma pack(push,1)`) guarantee that on‑disk structures fit within a page and have no padding.
- **RAII file handling** – `StorageManager` automatically closes the file.
- **Zero‑filled page allocation** – newly allocated pages are cleared.
- **Header page with magic number** (`0x12345678`) for simple file validation.
- **Scoped enums** (`enum class`) for type safety.
- **Variant‑based AST skeleton** (`ParsedStatement`) ready for a parser.
- **Self‑contained executable** – includes a minimal `main()` that demonstrates opening, allocating, and closing a DB file.
- **Compile‑time sanity checks** (`static_assert`) ensure struct sizes never exceed `PAGE_SIZE`.
- **Warnings silenced** – unused utility functions are marked `[[maybe_unused]]`.

---

## Prerequisites
- A **C++17** compatible compiler (e.g. `g++` ≥ 7, `clang++` ≥ 6).  
- POSIX‑compatible environment (macOS, Linux, WSL, etc.).  
- No external libraries – everything lives in a single source file.

---

## Building & Running

```bash
# Clone the repository (or copy tinydb.cpp into a directory)
git clone https://github.com/your‑username/tinydb.git
cd tinydb

# Build the executable (debug disabled, optimised)
g++ -std=c++17 -O2 -Wall -Wextra -o tinydb tinydb.cpp

# Run – a default database file `tinydb_test.db` will be created
./tinydb

# (Optional) Provide a custom file name
./tinydb mydata.db
```

**Typical output on first run**

```
Database file 'tinydb_test.db' opened successfully.
Current page count: 1
Allocated fresh page number: 1
New page count after allocation: 2
Database closed.
```

Running the program a second time yields

```
Database file 'tinydb_test.db' opened successfully.
Current page count: 2
Allocated fresh page number: 2
New page count after allocation: 3
Database closed.
```

The increasing page count proves that the file size (and therefore the page count) is persisted across executions.

---

## What the program does (demo)

1. **Open / create** `tinydb_test.db` in binary read/write mode.  
2. **Validate the header page** – if the file is brand‑new, it writes a 4 KB page where the first 4 bytes contain `MAGIC_NUMBER` (0x12345678).  
3. **Read the current page count** from the file size (`fileSize / PAGE_SIZE`).  
4. **Allocate a new page** – it increments the internal `pageCount`, writes a zero‑filled 4 KB block at the end of the file, and returns the new page number.  
5. **Print diagnostics** (current page count, allocated page number, new page count).  
6. **Close** the file via RAII.

Thus the program validates that the **storage layer works**, that pages can be allocated safely, and that the file state survives multiple runs.

---

## Design Overview

### On‑disk page layout
| Page | Purpose | Contents |
|------|---------|----------|
| **0** | Header | Magic number (`0x12345678`) + padding. |
| **1‑N** | Data pages | Used for B‑Tree nodes, records, catalog entries, etc. |  

All pages are exactly `PAGE_SIZE` = **4096** bytes.  

### `StorageManager` API
| Method | Description |
|--------|-------------|
| `open(const std::string&)` | Opens an existing DB file or creates a new one, initialises header page. |
| `close()` | Closes the underlying file handle. |
| `readPage(uint32_t pageNo, char* buf)` | Reads an entire page into a user‑provided buffer (must be `PAGE_SIZE`). |
| `writePage(uint32_t pageNo, const char* buf)` | Writes a full page from a buffer. |
| `allocatePage(uint32_t& pageNo)` | Appends a zero‑filled page to the file; returns its number. |
| `freePage(uint32_t pageNo)` | Stub for a future free‑list implementation. |
| `getPageCount() const` | Returns the number of pages currently stored. |

All error conditions return an `ErrorCode` enum; `errorMessage(ErrorCode)` translates it to a human‑readable string.

### Data structures (packed)
- `PageHeader`, `RecordHeader` – generic metadata for any page or record.
- `ColumnDefinition`, `TableMetadata` – schema definitions.
- `InteriorNode`, `LeafNode` – B‑Tree node structures (currently unused but ready).
- `SystemCatalog` – page‑type used to store table catalog entries.
- `RecordLocation` – helper for locating a record across pages.

All structures are `#pragma pack(push,1)` to ensure a **deterministic binary layout** and to guarantee they never exceed `PAGE_SIZE` (checked with `static_assert`).

### AST skeleton (`ParsedStatement`)
```cpp
struct ParsedStatement {
    StatementType type{StatementType::UNKNOWN};
    std::variant<
        std::unique_ptr<CreateTableStatement>,
        std::unique_ptr<InsertStatement>,
        std::unique_ptr<SelectStatement>
    > stmt;
};
```
Prepared for an eventual SQL parser; the variant handles memory automatically, eliminating manual `new`/`delete`.

---

## Extending tinydb

The repository intentionally stops after the storage manager, leaving **room for you** to build higher‑level functionality.

| Feature | Suggested implementation |
|----------|---------------------------|
| **SQL parser / REPL** | Use a simple hand‑rolled parser or a parser generator (e.g., `re2c`, `PEGTL`). Populate `ParsedStatement`. |
| **B‑Tree insertion & lookup** | Implement `insertKey`, `searchKey` based on `InteriorNode` and `LeafNode` structs. |
| **Catalog management** | Store `CatalogEntry` records in a dedicated catalog page (`SystemCatalog`). |
| **Record storage** | Serialize column values into the payload area of a page; update `RecordHeader`. |
| **Free‑list** | Extend `freePage` to maintain a list of reusable pages (e.g., a singly linked list stored in the `nextPage` field of `PageHeader`). |
| **Transaction & durability** | Add a write‑ahead log (WAL) or simple checkpointing. |
| **Concurrency** | Add a simple file lock or use `std::mutex` for in‑process thread safety. |
| **Tests** | Write unit tests (e.g., with GoogleTest or Catch2) for each storage manager operation and B‑Tree routine. |
| **Performance profiling** | Use `perf`, `valgrind`, or clang's sanitizers to measure and improve speed/memory usage. |

The existing code makes these extensions relatively straightforward because the low‑level file handling and layout are already abstracted.

---

## Testing & Validation

### Quick sanity checks

```bash
# Check file size after two runs (should be 3 pages = 12288 bytes)
ls -l tinydb_test.db
# => -rw-r--r--  1 user  staff  12288 ... tinydb_test.db

# Verify magic number at the very start of the file
od -t x1 -N 8 tinydb_test.db
# Expected output (little‑endian):
# 0000000 78 56 34 12 00 00 00 00
# ^ 0x12345678 magic, followed by 4 zero bytes
```

### Build‑time assertions
`static_assert` guarantees that each struct fits inside a page. If you change any struct or `PAGE_SIZE`, compilation will fail with a clear message.

### Runtime checks
All public methods of `StorageManager` return an `ErrorCode`. The test driver prints an error message if any operation fails.

---

## License
You can do whatever you like.

---

> **Tip:** Keep the repository single‑file‑focused (because i want so, do not care if you change it though). If you add significant new modules (e.g., a full parser), consider creating a new directory or a separate repository, but always keep the core `tinydb.cpp` functional and minimal.

---

### Have fun building the next generation of tiny embedded databases! 🚀
