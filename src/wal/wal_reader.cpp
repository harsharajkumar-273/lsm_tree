#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cerrno>

std::vector<WAL::Entry> WAL::recover() const {
    // Held for the whole read.
    //
    // recover() is const and previously took no lock, so it could open and read
    // path_ while clear() was unlinking and recreating that same file, or while
    // writeEntry() was appending to it. Either overlap hands recovery a file
    // that is being replaced underneath it — a partial read, or a read of a log
    // that no longer represents the engine's state.
    //
    // mu_ is mutable, so a const method can take it, and it is the same mutex
    // writeEntry() and clear() already hold.
    std::lock_guard<std::mutex> lock(mu_);

    // Recovery reads the entire WAL file into memory and parses entries.
    // Uses regular O_RDONLY (not O_DIRECT) since recovery is a one-time startup cost
    // and avoids O_DIRECT alignment constraints that complicate cross-block reads.
    int rfd = open(path_.c_str(), O_RDONLY);
    if (rfd < 0) return {};

    // Refuse a log too large to hold, rather than being killed trying.
    //
    // Recovery reads the whole file into one contiguous buffer, so a multi-GB
    // log means a multi-GB allocation. The failure mode without this check is
    // the OOM killer terminating the process mid-startup — no diagnostic, no
    // indication which file was responsible, and a restart loop that repeats it.
    //
    // A log should never approach this size in normal operation: flushMemtable()
    // calls wal_.clear() after every flush, so the log is bounded by the
    // memtable threshold. A file this large means flushes have been failing, and
    // that is the condition worth surfacing — the size is the symptom.
    //
    // The bound is checked with fstat before any allocation, so an oversized log
    // costs nothing to reject.
    struct stat st;
    if (fstat(rfd, &st) == 0 && static_cast<uint64_t>(st.st_size) > MAX_RECOVERY_BYTES) {
        const off_t size = st.st_size;
        close(rfd);
        throw std::runtime_error(
            "WAL: refusing to recover " + std::to_string(size) + " byte log; the limit is " +
            std::to_string(MAX_RECOVERY_BYTES) +
            " bytes. A log this large means memtable flushes have not been "
            "clearing it — investigate flush failures before recovering.");
    }

    // Read entire file into a contiguous buffer
    std::vector<char> buf;
    char tmp[4096];
    ssize_t bytes;
    while (true) {
        bytes = read(rfd, tmp, sizeof(tmp));

        // A signal delivered mid-read returns -1/EINTR without having failed.
        // The old loop exited on any non-positive result, so an interrupted
        // read silently truncated the log — recovery then treated whatever had
        // been read so far as the whole file and discarded every entry after
        // the interruption. Retrying is the correct response; the read has not
        // actually gone wrong.
        if (bytes < 0) {
            if (errno == EINTR) continue;

            // A real read error. Returning what we have would present a partial
            // log as a complete one, so refuse instead.
            const int err = errno;
            close(rfd);
            throw std::runtime_error(std::string("WAL: read failed during recovery: ") +
                                     std::strerror(err));
        }

        if (bytes == 0) break;   // end of file

        // Backstop for the fstat check above: a log being appended to while
        // recovery reads it can outgrow the size reported at open, and fstat
        // can fail outright on an unusual filesystem.
        if (buf.size() + static_cast<size_t>(bytes) > MAX_RECOVERY_BYTES) {
            close(rfd);
            throw std::runtime_error(
                "WAL: log exceeded the " + std::to_string(MAX_RECOVERY_BYTES) +
                " byte recovery limit while being read");
        }

        buf.insert(buf.end(), tmp, tmp + bytes);
    }
    close(rfd);

    std::vector<Entry> entries;
    size_t offset = 0;
    size_t total = buf.size();

    while (offset < total) {
        // Each WAL entry is written as a 512-byte-aligned block (for O_DIRECT).
        // Record the start so we can skip padding after reading the entry.
        size_t entry_start = offset;

        if (offset + 1 > total) break;
        uint8_t type = static_cast<uint8_t>(buf[offset++]);
        if (type == 0) {
            // Hit padding — skip to next 512-byte boundary
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        // A truncated or corrupted length header is skipped, not fatal.
        //
        // Both checks below used to `break`, abandoning the remainder of the
        // log and discarding every valid entry after the damaged one — the
        // opposite of what recovery is for. It was also inconsistent with how
        // this same loop already handles its two other corruption cases: the
        // padding branch above and the CRC mismatch below both advance to the
        // next 512-byte boundary and continue.
        //
        // Entries are written as 512-byte aligned blocks, so that boundary is
        // where the next record begins. Advancing there resynchronises with the
        // record stream rather than giving up on it, and always moves forward,
        // so the loop still terminates.
        if (offset + 8 > total) {
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        uint32_t klen, vlen;
        std::memcpy(&klen, buf.data() + offset, 4); offset += 4;
        std::memcpy(&vlen, buf.data() + offset, 4); offset += 4;

        if (offset + klen + vlen + 4 > total) {
            std::cerr << "WAL: entry at offset " << entry_start
                      << " declares a length past the end of the log (klen=" << klen
                      << ", vlen=" << vlen << "). Skipping corrupted entry.\n";
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        std::string key(buf.data() + offset, klen); offset += klen;
        std::string value(buf.data() + offset, vlen); offset += vlen;

        uint32_t stored_crc;
        std::memcpy(&stored_crc, buf.data() + offset, 4); offset += 4;

        // Validate CRC before accepting the entry
        uint32_t expected_crc = crc32(type, key, value);
        if (stored_crc != expected_crc) {
            std::cerr << "WAL: CRC mismatch for key '" << key
                      << "' (stored=0x" << std::hex << stored_crc
                      << ", expected=0x" << expected_crc << std::dec
                      << "). Skipping corrupted entry.\n";
            // Skip to next aligned boundary
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        Entry e;
        // Only the two defined type bytes are accepted.
        //
        // This was `(type == 0x01) ? PUT : DEL`, so every byte that wasn't 0x01
        // — 0x00, 0x03, 0xFF, anything — became a deletion. A record that
        // passes CRC but carries an unrecognised type is a record written by a
        // format this build doesn't know, and turning it into a tombstone
        // deletes a key the log never asked to delete. Skipping to the next
        // block is the same treatment the CRC-mismatch path gives.
        if (type != 0x01 && type != 0x02) {
            std::cerr << "WAL: entry at offset " << entry_start
                      << " has unrecognised type byte 0x" << std::hex
                      << static_cast<int>(type) << std::dec
                      << "; skipping rather than treating it as a delete.\n";
            offset = ((entry_start / 512) + 1) * 512;
            continue;
        }

        e.type = (type == 0x01) ? Entry::Type::PUT : Entry::Type::DEL;
        e.key = key;
        e.value = value;
        entries.push_back(std::move(e));

        // Advance to next 512-byte boundary (skip O_DIRECT padding)
        size_t entry_size = 1 + 4 + 4 + klen + vlen + 4;
        size_t aligned_size = (entry_size + 511) & ~511;
        offset = entry_start + aligned_size;
    }
    return entries;
}
