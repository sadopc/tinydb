/*****************************************************************************************
 * tinydb.cpp – a minimal, single‑file SQLite‑style engine (core definitions only)
 *
 *  * The file is now a complete program (adds a tiny `main()`).
 *  * Unused‑function warnings are silenced with [[maybe_unused]].
 *  * The rest of the code is unchanged apart from tiny style tweaks.
 *
 * Compile:
 *   g++ -std=c++17 -O2 -Wall -Wextra -o tinydb tinydb.cpp
 *
 *****************************************************************************************/

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <variant>
#include <memory>
#include <cassert>

// -----------------------------------------------------------------------------
// Compile‑time constants (constexpr for better optimisation)
// -----------------------------------------------------------------------------
constexpr uint32_t PAGE_SIZE               = 4096;
constexpr uint32_t MAGIC_NUMBER            = 0x12345678;
constexpr uint32_t MAX_IDENTIFIER_LENGTH   = 64;
constexpr uint32_t MAX_COLUMNS             = 32;

// -----------------------------------------------------------------------------
// Scoped enumerations (strongly typed)
// -----------------------------------------------------------------------------
enum class ErrorCode : uint32_t {
    SUCCESS                 = 0,
    FILE_IO_ERROR           = 1,
    PAGE_ALLOCATION_FAILURE = 2,
    INVALID_INPUT           = 3,
    OUT_OF_MEMORY           = 4
};
enum class StatementType : uint32_t {
    CREATE_TABLE = 0,
    INSERT       = 1,
    SELECT       = 2,
    UNKNOWN      = 3
};
enum class DataType : uint32_t {
    INTEGER = 0,
    STRING  = 1,
    FLOAT   = 2,
    DOUBLE  = 3
};
enum class PageType : uint32_t {
    HEADER    = 0,   // Reserved page (stores magic number and DB header)
    LEAF      = 1,
    INTERIOR  = 2,
    CATALOG   = 3
};
enum class RecordFlag : uint32_t {
    LIVE    = 0,
    DELETED = 1
};

// -----------------------------------------------------------------------------
// Helper utilities (marked [[maybe_unused]] because they are not used yet)
// -----------------------------------------------------------------------------
[[maybe_unused]] static std::string errorMessage(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS:                 return "Success";
        case ErrorCode::FILE_IO_ERROR:           return "File I/O error";
        case ErrorCode::PAGE_ALLOCATION_FAILURE: return "Page allocation failure";
        case ErrorCode::INVALID_INPUT:           return "Invalid input";
        case ErrorCode::OUT_OF_MEMORY:           return "Out of memory";
        default:                                 return "Unknown error";
    }
}
[[maybe_unused]] static std::string toUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c){ return std::toupper(c); });
    return result;
}
[[maybe_unused]] static std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() &&
           std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    size_t end = str.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    return str.substr(start, end - start);
}

// -----------------------------------------------------------------------------
// On‑disk structures – packed to guarantee layout size
// -----------------------------------------------------------------------------
#pragma pack(push, 1)
struct PageHeader {
    uint32_t pageType;   // PageType (stored as uint32_t)
    uint32_t nextPage;   // Overflow/next page number (0 if none)
    uint32_t entryCount; // Number of entries stored on the page
};
struct RecordHeader {
    uint32_t recordFlag;    // RecordFlag (0 = live, 1 = deleted)
    uint32_t payloadSize;   // Size of payload in bytes
    uint32_t overflowPage;  // First overflow page (0 if none)
};
struct ColumnDefinition {
    char columnName[MAX_IDENTIFIER_LENGTH]; // NUL‑terminated column name
    uint32_t dataType;                      // DataType (as uint32_t)
    uint32_t dataSize;                      // Fixed size for the column (e.g., string max length)
};
struct TableMetadata {
    char tableName[MAX_IDENTIFIER_LENGTH]; // NUL‑terminated table name
    uint32_t columnCount;                  // Number of columns
    uint32_t rootPageNumber;               // Root page of the table's B‑Tree
    ColumnDefinition columns[MAX_COLUMNS]; // Column definitions
};
struct CatalogEntry {
    char tableName[MAX_IDENTIFIER_LENGTH]; // NUL‑terminated table name
    uint32_t rootPageNumber;               // Root page of the table's B‑Tree
    uint32_t columnCount;                  // Number of columns
    // Column definitions are stored in the record payload that follows this header
};
struct InteriorNode {
    PageHeader header;                     // Common page header
    uint32_t keyCount;                     // Number of occupied key slots
    uint32_t keys[MAX_COLUMNS];            // Keys (max limited by page size)
    uint32_t childPointers[MAX_COLUMNS + 1]; // Child page numbers (n+1 for n keys)
};
struct LeafNode {
    PageHeader header;                     // Common page header
    uint32_t recordCount;                  // Number of occupied record slots
    uint32_t keys[MAX_COLUMNS];            // Keys (same limit as interior)
    uint32_t recordOffsets[MAX_COLUMNS];   // Offsets of records within the page
};
struct SystemCatalog {
    PageHeader header;                     // Page header (type = CATALOG)
    uint32_t entryCount;                   // Number of catalog entries on this page
    uint32_t rootPageNumber;               // Root page number of catalog B‑Tree
    // Actual catalog records follow in the page body
};
#pragma pack(pop)

// -----------------------------------------------------------------------------
// Compile‑time sanity checks
// -----------------------------------------------------------------------------
static_assert(sizeof(PageHeader)   <= PAGE_SIZE, "PageHeader exceeds PAGE_SIZE");
static_assert(sizeof(RecordHeader) <= PAGE_SIZE, "RecordHeader exceeds PAGE_SIZE");
static_assert(sizeof(InteriorNode) <= PAGE_SIZE, "InteriorNode exceeds PAGE_SIZE");
static_assert(sizeof(LeafNode)     <= PAGE_SIZE, "LeafNode exceeds PAGE_SIZE");

// -----------------------------------------------------------------------------
// B‑Tree layout constants (derived from page size and packed structs)
// -----------------------------------------------------------------------------
constexpr uint32_t KEY_PAIR_SIZE = sizeof(uint32_t) + sizeof(uint32_t); // key + pointer/record offset
constexpr uint32_t MAX_KEYS      = (PAGE_SIZE - sizeof(PageHeader) - sizeof(uint32_t)) / KEY_PAIR_SIZE;
constexpr uint32_t MAX_RECORDS   = MAX_KEYS; // For simplicity leaf and interior share the same limit
constexpr uint32_t MIN_KEYS      = MAX_KEYS / 2;

// -----------------------------------------------------------------------------
// Record location helper
// -----------------------------------------------------------------------------
struct RecordLocation {
    uint32_t pageNumber{};
    uint32_t offset{};
    bool     found{false};
    RecordLocation() = default;
    RecordLocation(uint32_t page, uint32_t off, bool f = true)
        : pageNumber(page), offset(off), found(f) {}
};

// -----------------------------------------------------------------------------
// Abstract syntax tree (AST) structures for parsed SQL statements
// -----------------------------------------------------------------------------
struct CreateTableStatement {
    std::string tableName;
    std::vector<ColumnDefinition> columns;
};
struct InsertStatement {
    std::string tableName;
    std::vector<std::string> columnNames;
    std::vector<std::string> values;
};
struct SelectStatement {
    std::string tableName;
    std::vector<std::string> columnNames;
    std::string whereColumn;
    std::string whereValue;
};

// ParsedStatement – uses std::variant for type‑safe storage
struct ParsedStatement {
    StatementType type{StatementType::UNKNOWN};
    std::variant<
        std::unique_ptr<CreateTableStatement>,
        std::unique_ptr<InsertStatement>,
        std::unique_ptr<SelectStatement>
    > stmt;
    ParsedStatement() = default;
    ~ParsedStatement() = default; // variant members clean themselves up automatically
};

// -----------------------------------------------------------------------------
// StorageManager – RAII wrapper around the database file
// -----------------------------------------------------------------------------
class StorageManager {
private:
    std::fstream file;          // Binary file handle
    std::string filename;       // Database file name
    uint32_t    pageCount{0};   // Number of pages currently in the file

    // Helper to write a fully zero‑filled page (used during allocation)
    ErrorCode writeZeroPage(uint32_t pageNumber) {
        static const std::vector<char> zeroPage(PAGE_SIZE, 0);
        return writePage(pageNumber, zeroPage.data());
    }

public:
    StorageManager() = default;
    ~StorageManager() { close(); }   // RAII: ensure file is closed

    // -----------------------------------------------------------------
    // Open (or create) a database file
    // -----------------------------------------------------------------
    ErrorCode open(const std::string& fname) {
        filename = fname;
        // Try opening existing file first
        file.open(filename,
                 std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            // File does not exist – create a fresh one
            file.clear();
            file.open(filename,
                     std::ios::out | std::ios::binary);
            if (!file.is_open())
                return ErrorCode::FILE_IO_ERROR;
            file.close();

            // Re‑open for read/write
            file.open(filename,
                     std::ios::in | std::ios::out | std::ios::binary);
            if (!file.is_open())
                return ErrorCode::FILE_IO_ERROR;

            // Reserve page 0 for DB header (magic number). Initialise it now.
            pageCount = 1; // page 0 exists
            char header[PAGE_SIZE] = {0};
            std::memcpy(header, &MAGIC_NUMBER, sizeof(MAGIC_NUMBER));
            file.seekp(0, std::ios::beg);
            file.write(header, PAGE_SIZE);
            file.flush();
        } else {
            // Existing file – determine page count
            file.seekg(0, std::ios::end);
            std::streampos fileSize = file.tellg();
            if (fileSize % PAGE_SIZE != 0) {
                // Corrupted file: size not multiple of PAGE_SIZE
                return ErrorCode::FILE_IO_ERROR;
            }
            pageCount = static_cast<uint32_t>(fileSize / PAGE_SIZE);
        }
        return ErrorCode::SUCCESS;
    }

    // -----------------------------------------------------------------
    // Close the database file
    // -----------------------------------------------------------------
    ErrorCode close() {
        if (file.is_open())
            file.close();
        return ErrorCode::SUCCESS;
    }

    // -----------------------------------------------------------------
    // Read a page into caller‑provided buffer (must be PAGE_SIZE bytes)
    // -----------------------------------------------------------------
    ErrorCode readPage(uint32_t pageNumber, char* buffer) {
        if (!file.is_open() || buffer == nullptr)
            return ErrorCode::INVALID_INPUT;
        if (pageNumber >= pageCount)
            return ErrorCode::INVALID_INPUT;
        file.seekg(pageNumber * PAGE_SIZE, std::ios::beg);
        if (file.fail())
            return ErrorCode::FILE_IO_ERROR;
        file.read(buffer, PAGE_SIZE);
        if (file.fail() && !file.eof())
            return ErrorCode::FILE_IO_ERROR;
        return ErrorCode::SUCCESS;
    }

    // -----------------------------------------------------------------
    // Write a page from caller‑provided buffer (must be PAGE_SIZE bytes)
    // -----------------------------------------------------------------
    ErrorCode writePage(uint32_t pageNumber, const char* buffer) {
        if (!file.is_open() || buffer == nullptr)
            return ErrorCode::INVALID_INPUT;
        if (pageNumber >= pageCount)   // cannot write past the current end
            return ErrorCode::INVALID_INPUT;
        file.seekp(pageNumber * PAGE_SIZE, std::ios::beg);
        if (file.fail())
            return ErrorCode::FILE_IO_ERROR;
        file.write(buffer, PAGE_SIZE);
        if (file.fail())
            return ErrorCode::FILE_IO_ERROR;
        file.flush();
        return ErrorCode::SUCCESS;
    }

    // -----------------------------------------------------------------
    // Allocate a fresh page and return its page number
    // -----------------------------------------------------------------
    ErrorCode allocatePage(uint32_t& pageNumber) {
        if (!file.is_open())
            return ErrorCode::FILE_IO_ERROR;
        pageNumber = pageCount;
        ++pageCount;
        // Extend the file by one full page (zero‑filled)
        return writeZeroPage(pageNumber);
    }

    // -----------------------------------------------------------------
    // Free a page (stub – free‑list implementation pending)
    // -----------------------------------------------------------------
    ErrorCode freePage(uint32_t pageNumber) {
        if (!file.is_open())
            return ErrorCode::FILE_IO_ERROR;
        if (pageNumber >= pageCount)
            return ErrorCode::INVALID_INPUT;
        // TODO: add page‑free list handling here.
        return ErrorCode::SUCCESS;
    }

    // -----------------------------------------------------------------
    // Retrieve the current page count (useful for diagnostics)
    // -----------------------------------------------------------------
    uint32_t getPageCount() const { return pageCount; }
};

// -----------------------------------------------------------------------------
// Minimal test driver – provides the missing `main()`
// -----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    const char* dbFile = (argc > 1) ? argv[1] : "tinydb_test.db";

    StorageManager storage;
    if (auto rc = storage.open(dbFile); rc != ErrorCode::SUCCESS) {
        std::cerr << "Failed to open/create database '" << dbFile
                  << "': " << errorMessage(rc) << "\n";
        return 1;
    }

    std::cout << "Database file '" << dbFile << "' opened successfully.\n";
    std::cout << "Current page count: " << storage.getPageCount() << "\n";

    // Allocate a new page just to prove that the manager works.
    uint32_t newPage;
    if (auto rc = storage.allocatePage(newPage); rc != ErrorCode::SUCCESS) {
        std::cerr << "Page allocation failed: " << errorMessage(rc) << "\n";
        storage.close();
        return 1;
    }

    std::cout << "Allocated fresh page number: " << newPage << "\n";
    std::cout << "New page count after allocation: " << storage.getPageCount() << "\n";

    storage.close();
    std::cout << "Database closed.\n";
    return 0;
}
