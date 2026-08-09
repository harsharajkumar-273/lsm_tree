// Format versioning tests (issue #50).
//
// The WAL and SSTable carried no magic number and no version field, so a reader
// had no way to tell one layout from another. Any future format change would
// have made existing files parse as garbage rather than be rejected.
//
// These cover what the maintainer asked for on the issue: v1 detection, v2
// detection, mismatched magic, unknown versions, and malformed or truncated
// headers failing clearly rather than falling through into record parsing.
//
// Byte order is deliberately unchanged — see docs/FORMAT.md.

#include "../src/format.h"
#include "../src/sstable/sstable.h"
#include "../src/wal/wal.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}
void check(bool ok, const std::string& test, const std::string& msg = "assertion failed") {
    ok ? pass(test) : fail(test, msg);
}

namespace {

std::string readAllBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void writeBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// True when the callable throws. Used rather than asserting on the message,
// which would make these tests break on rewording rather than on behaviour.
template <typename F>
bool refuses(F&& f) {
    try {
        f();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

const std::string kDir = "/tmp/lsm_format_tests";

std::string p(const std::string& name) { return kDir + "/" + name; }

}  // namespace

// ── SSTable ───────────────────────────────────────────────────────────────

void testSSTableWritesV2() {
    const std::string path = p("v2.sst");
    SSTable::write(path, {{"a", std::string("1")},
                          {"b", std::string("2")},
                          {"c", std::nullopt}});

    SSTable table(path);
    check(table.formatVersion() == lsm::format::kVersion2,
          "SSTable: a newly written file reports version 2");

    // The magic sits in the last eight bytes, after the two offsets, so that
    // the reader can still find the offsets by seeking from the end.
    const std::string bytes = readAllBytes(path);
    uint32_t magic = 0, version = 0;
    std::memcpy(&magic, bytes.data() + bytes.size() - 8, 4);
    std::memcpy(&version, bytes.data() + bytes.size() - 4, 4);

    check(magic == lsm::format::kSSTableMagic,
          "SSTable: the magic is present in the trailer");
    check(version == lsm::format::kVersion2,
          "SSTable: the version is present in the trailer");
}

void testSSTableV2ReadsBack() {
    const std::string path = p("v2.sst");
    SSTable table(path);

    const auto a = table.get("a");
    check(a.has_value() && *a == "1", "SSTable: v2 get() returns the value");
    check(table.readAll().size() == 3, "SSTable: v2 readAll() finds every entry");
}

void testSSTableV1StillOpens() {
    // A v1 file is byte-for-byte a v2 file without its last eight bytes: the
    // format is identical apart from the identification.
    const std::string v2 = readAllBytes(p("v2.sst"));
    const std::string path = p("v1.sst");
    writeBytes(path, v2.substr(0, v2.size() - 8));

    SSTable table(path);
    check(table.formatVersion() == lsm::format::kVersion1,
          "SSTable: a file with no magic is detected as v1");

    const auto b = table.get("b");
    check(b.has_value() && *b == "2",
          "SSTable: v1 get() still works — existing files keep reading");
    check(table.readAll().size() == 3,
          "SSTable: v1 readAll() still works");
}

void testSSTableUnknownVersionIsRefused() {
    std::string bytes = readAllBytes(p("v2.sst"));
    const uint32_t future = 99;
    std::memcpy(&bytes[bytes.size() - 4], &future, 4);

    const std::string path = p("badver.sst");
    writeBytes(path, bytes);

    check(refuses([&] { SSTable table(path); }),
          "SSTable: an unknown version is refused rather than guessed at");
}

void testSSTableForeignMagicIsNotMistakenForV2() {
    // A WAL magic in an SSTable trailer must not be accepted. The two differ so
    // that one file type cannot be opened as the other.
    std::string bytes = readAllBytes(p("v2.sst"));
    const uint32_t wal = lsm::format::kWalMagic;
    std::memcpy(&bytes[bytes.size() - 8], &wal, 4);

    const std::string path = p("foreign.sst");
    writeBytes(path, bytes);

    // Not an SSTable magic, so detection falls back to v1 and seeks 16 bytes
    // from the end. On a file that is physically v2 that lands on bloom_offset
    // and the magic rather than on the two real offsets, so the values are
    // nonsense — and the existing bounds check refuses them.
    //
    // That is the outcome worth pinning: a WAL magic must not open an SSTable,
    // and the failure is a clear error rather than a seek to an arbitrary
    // position. I first wrote this expecting the file to open; running it
    // showed the bounds check catches the case, which is the better behaviour.
    check(refuses([&] { SSTable table(path); }),
          "SSTable: a WAL magic does not open as an SSTable");
}

void testSSTableTooSmallIsRefused() {
    const std::string path = p("tiny.sst");
    writeBytes(path, std::string(4, '\0'));

    check(refuses([&] { SSTable table(path); }),
          "SSTable: a file shorter than a trailer is refused");
}

// ── WAL ───────────────────────────────────────────────────────────────────

void testWalWritesV2Header() {
    const std::string path = p("v2.wal");
    ::unlink(path.c_str());

    {
        WAL wal(path);
        wal.logPut("alpha", "1");
    }

    const std::string bytes = readAllBytes(path);
    uint32_t magic = 0, version = 0;
    std::memcpy(&magic, bytes.data(), 4);
    std::memcpy(&version, bytes.data() + 4, 4);

    check(magic == lsm::format::kWalMagic, "WAL: the magic is at offset 0");
    check(version == lsm::format::kVersion2, "WAL: the version follows the magic");
    check(bytes.size() >= lsm::format::kWalHeaderBytes,
          "WAL: the header occupies a whole block");
}

void testWalHeaderKeepsRecordsAligned() {
    // The header is 512 bytes rather than the 16 originally proposed, for two
    // reasons: the log is opened O_DIRECT, which cannot write a 16-byte block
    // at all, and a whole block keeps every record on the same 512-byte grid
    // that recovery's resynchronisation arithmetic assumes.
    const std::string bytes = readAllBytes(p("v2.wal"));

    check(lsm::format::kWalHeaderBytes % 512 == 0,
          "WAL: the header is a whole number of 512-byte blocks");
    check(bytes.size() % 512 == 0,
          "WAL: the log stays 512-aligned with the header in place");
}

void testWalV2Recovers() {
    const std::string path = p("recover.wal");
    ::unlink(path.c_str());

    // Written and closed before recovering: io_uring writes are asynchronous,
    // so recovering from a live WAL races with whatever is still in flight.
    {
        WAL wal(path);
        wal.logPut("alpha", "1");
        wal.logPut("beta", "2");
        wal.logDel("alpha");
    }

    WAL wal(path);
    const auto entries = wal.recover();

    check(entries.size() == 3, "WAL: every record in a v2 log is recovered");
    check(entries[0].key == "alpha" && entries[0].value == "1",
          "WAL: v2 record contents survive the round trip");
    check(entries[2].type == WAL::Entry::Type::DEL,
          "WAL: a delete is recovered as a delete");
}

void testWalV1StillRecovers() {
    // A v1 log is a v2 log with the header removed: records begin at offset 0.
    const std::string v2 = readAllBytes(p("recover.wal"));
    const std::string path = p("v1.wal");
    writeBytes(path, v2.substr(lsm::format::kWalHeaderBytes));

    WAL wal(path);
    const auto entries = wal.recover();

    check(entries.size() == 3,
          "WAL: a v1 log recovers every record — existing logs keep working");
    check(entries[1].key == "beta", "WAL: v1 record contents are intact");
}

void testWalTruncatedHeaderIsRefused() {
    // The case the maintainer called out: a malformed header must fail clearly
    // rather than fall through into record parsing. Falling through would hand
    // the loop a partial header and resynchronise it onto whatever followed.
    const std::string path = p("trunc.wal");
    std::string bytes(8, '\0');
    const uint32_t magic = lsm::format::kWalMagic;
    std::memcpy(&bytes[0], &magic, 4);
    writeBytes(path, bytes);

    check(refuses([&] { WAL wal(path); wal.recover(); }),
          "WAL: a log truncated inside its header is refused, not parsed");
}

void testWalUnknownVersionIsRefused() {
    std::string bytes = readAllBytes(p("recover.wal"));
    const uint32_t future = 99;
    std::memcpy(&bytes[4], &future, 4);

    const std::string path = p("badver.wal");
    writeBytes(path, bytes);

    check(refuses([&] { WAL wal(path); wal.recover(); }),
          "WAL: an unknown version is refused rather than guessed at");
}

void testWalClearRewritesTheHeader() {
    // clear() removes and recreates the log, so the header has to be written
    // again. Missing this would leave a v1 log behind a v2 writer.
    const std::string path = p("cleared.wal");
    ::unlink(path.c_str());

    {
        WAL wal(path);
        wal.logPut("before", "clear");
        wal.clear();
        wal.logPut("after", "clear");
    }

    WAL wal(path);
    check(wal.recover().size() == 1, "WAL: only post-clear records remain");

    uint32_t magic = 0;
    std::memcpy(&magic, readAllBytes(path).data(), 4);
    check(magic == lsm::format::kWalMagic,
          "WAL: the header is rewritten after clear()");
}

// ── constants ─────────────────────────────────────────────────────────────

void testTheTwoMagicsDiffer() {
    check(lsm::format::kWalMagic != lsm::format::kSSTableMagic,
          "format: the WAL and SSTable magics differ");
}

void testTheCurrentVersionIsTwo() {
    check(lsm::format::kCurrentVersion == lsm::format::kVersion2,
          "format: the writer emits version 2");
}

void testTheFooterGrewByEight() {
    check(lsm::format::kSSTableFooterV2Bytes ==
              lsm::format::kSSTableFooterV1Bytes + 8,
          "format: the v2 trailer adds exactly a magic and a version");
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: On-Disk Format Version\n";
    std::cout << "===========================================\n";

    std::filesystem::remove_all(kDir);
    std::filesystem::create_directories(kDir);

    std::cout << "\n-- SSTable --\n";
    testSSTableWritesV2();
    testSSTableV2ReadsBack();
    testSSTableV1StillOpens();
    testSSTableUnknownVersionIsRefused();
    testSSTableForeignMagicIsNotMistakenForV2();
    testSSTableTooSmallIsRefused();

    std::cout << "\n-- WAL --\n";
    testWalWritesV2Header();
    testWalHeaderKeepsRecordsAligned();
    testWalV2Recovers();
    testWalV1StillRecovers();
    testWalTruncatedHeaderIsRefused();
    testWalUnknownVersionIsRefused();
    testWalClearRewritesTheHeader();

    std::cout << "\n-- constants --\n";
    testTheTwoMagicsDiffer();
    testTheCurrentVersionIsTwo();
    testTheFooterGrewByEight();

    std::filesystem::remove_all(kDir);

    std::cout << "\n  All format versioning tests passed.\n";
    return 0;
}
