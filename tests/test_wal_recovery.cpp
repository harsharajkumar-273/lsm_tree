#include "../src/db/lsm_engine.h"
#include "../src/wal/wal.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

void pass(const std::string& test) {
    std::cout << "  \033[32m✓\033[0m " << test << "\n";
}
void fail(const std::string& test, const std::string& msg) {
    std::cout << "  \033[31m✗\033[0m " << test << " — " << msg << "\n";
    std::exit(1);
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Running Unit Test: WAL Recovery & Checks\n";
    std::cout << "===========================================\n";

    // Write the log directly rather than through the engine.
    //
    // This block and the corruption block below both need a WAL that still
    // holds its records. Destroying an LSMEngine no longer leaves one, because
    // shutdown now flushes — so producing the log through the WAL itself is
    // what actually models a process that died before flushing.
    std::filesystem::remove_all("/tmp/lsm_test_wal");
    std::filesystem::create_directories("/tmp/lsm_test_wal");
    {
        WAL wal("/tmp/lsm_test_wal/wal.log");
        wal.logPut("keyA", "dataA");
        wal.logPut("keyB", "dataB");
    }

    // Re-open and verify recovered entries
    {
        LSMEngine db("/tmp/lsm_test_wal");
        auto v = db.get("keyA");
        if (!v || *v != "dataA") fail("WAL recovery", "Expected keyA to hold 'dataA'");
        
        v = db.get("keyB");
        if (!v || *v != "dataB") fail("WAL recovery", "Expected keyB to hold 'dataB'");
        pass("Durable Log Replay");
    }

    // CRC Corruption Test
    std::filesystem::remove_all("/tmp/lsm_test_wal_corrupt");
    std::filesystem::create_directories("/tmp/lsm_test_wal_corrupt");
    {
        WAL wal("/tmp/lsm_test_wal_corrupt/wal.log");
        wal.logPut("safe_key", "good_data");
        wal.logPut("corrupt_key", "corrupt_data");
    }

    // Corrupt a byte in the second entry of the WAL file.
    //
    // Every step here is checked. Previously open(), lseek() and write() all
    // discarded their results and the whole block was wrapped in `if (fd >= 0)`,
    // so any failure meant the file was silently left intact — and the
    // assertions below would then pass against an uncorrupted log, reporting
    // success for a check that never ran. A test that cannot fail is worse than
    // no test, because it is counted as coverage.
    std::string wal_path = "/tmp/lsm_test_wal_corrupt/wal.log";
    int fd = open(wal_path.c_str(), O_RDWR);
    if (fd < 0) {
        fail("CRC Corruption", std::string("cannot open WAL to corrupt it: ") + std::strerror(errno));
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        close(fd);
        fail("CRC Corruption", std::string("lseek to end failed: ") + std::strerror(errno));
    }
    if (size < 1024) {
        close(fd);
        fail("CRC Corruption",
             "WAL is " + std::to_string(size) + " bytes; expected at least two 512-byte blocks, "
             "so there is no second entry to corrupt");
    }

    if (lseek(fd, 512, SEEK_SET) != 512) {
        close(fd);
        fail("CRC Corruption", std::string("lseek to offset 512 failed: ") + std::strerror(errno));
    }

    char corrupt_byte = 0x03; // Invalid type to cause CRC mismatch
    const ssize_t written = write(fd, &corrupt_byte, 1);
    if (written != 1) {
        close(fd);
        fail("CRC Corruption",
             "write of corruption byte returned " + std::to_string(written) + ": " +
             std::strerror(errno));
    }

    if (close(fd) != 0) {
        fail("CRC Corruption", std::string("close after corrupting failed: ") + std::strerror(errno));
    }

    // Reopen. The engine should skip the corrupt second entry but keep the safe one
    {
        LSMEngine db("/tmp/lsm_test_wal_corrupt");
        auto v = db.get("safe_key");
        if (!v || *v != "good_data") {
            fail("CRC Corruption", "Expected safe_key to still be recovered correctly");
        }
        
        v = db.get("corrupt_key");
        if (v.has_value()) {
            fail("CRC Corruption", "Expected corrupted key to be skipped during recovery");
        }
        pass("Checksum Verification: corrupted entries successfully skipped");
    }

    // Fast Destruction In-Flight Completion Test (Issue #120)
    std::filesystem::remove_all("/tmp/lsm_test_wal_inflight");
    std::filesystem::create_directories("/tmp/lsm_test_wal_inflight");
    std::string test_log_path = "/tmp/lsm_test_wal_inflight/wal.log";
    {
        WAL wal(test_log_path);
        for (int i = 0; i < 50; ++i) {
            wal.logPut("k" + std::to_string(i), "v" + std::to_string(i));
        }
        // Destruct wal immediately without sleep to ensure drain() harvests all 50 CQEs
    }
    {
        WAL wal(test_log_path);
        auto recovered = wal.recover();
        if (recovered.size() != 50) {
            fail("In-Flight WAL Drain", "Expected 50 entries recovered but got " + std::to_string(recovered.size()));
        }
        pass("Fast Destructor Drain: recovered 50 of 50 in-flight entries");
    }

    std::cout << "\033[32mWAL Recovery tests passed successfully.\033[0m\n\n";
    return 0;
}
