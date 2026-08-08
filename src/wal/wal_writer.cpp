#include "wal.h"
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <iostream>
#include <filesystem>

WAL::WAL(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
    if (fd_ < 0) throw std::runtime_error("WAL: cannot open file with O_DIRECT: " + path);

    if (io_uring_queue_init(8, &ring_, 0) < 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("WAL: failed to init io_uring");
    }
    ring_ready_ = true;
}

WAL::~WAL() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        drain();
    }
    // Ensure all writes are durable on disk before closing.
    if (fd_ >= 0) fdatasync(fd_);
    if (ring_ready_) {
        io_uring_queue_exit(&ring_);
        ring_ready_ = false;
    }
    if (fd_ >= 0) close(fd_);
}

void WAL::logPut(const std::string& key, const std::string& value) {
    writeEntry(Entry::Type::PUT, key, value);
}

void WAL::logDel(const std::string& key) {
    writeEntry(Entry::Type::DEL, key, "");
}

void WAL::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    drain();                     // drain all pending in-flight CQEs synchronously

    // Every handle is marked released as it is released, and the removal uses
    // the non-throwing overload. The previous ordering exited the ring and
    // closed the descriptor and *then* called the throwing form of remove(),
    // so a failed removal escaped with the ring already exited and fd_ still
    // holding a closed descriptor — after which the destructor exited the ring
    // a second time and closed the descriptor again.
    if (ring_ready_) {
        io_uring_queue_exit(&ring_);
        ring_ready_ = false;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    std::error_code ec;
    std::filesystem::remove(path_, ec);
    if (ec) {
        throw std::runtime_error("WAL: failed to remove log during clear: " + ec.message());
    }

    fd_ = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("WAL: cannot reopen log after clear: " + path_);
    }

    if (io_uring_queue_init(8, &ring_, 0) < 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("WAL: failed to re-init io_uring after clear");
    }
    ring_ready_ = true;
    pending_cqes_ = 0;
}

void WAL::writeEntry(Entry::Type type, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);

    // A completion has already reported a failed or short write, so the log on
    // disk no longer describes what the caller was told was committed. Refuse
    // further entries rather than appending to a record stream with a hole in
    // it.
    if (!failure_.empty()) {
        throw std::runtime_error("WAL: refusing to write, " + failure_);
    }
    // Lengths are stored as uint32_t on disk, so anything that will not fit is
    // refused rather than silently truncated.
    //
    // `uint32_t klen = key.size()` is a narrowing conversion with no cast and no
    // diagnostic: a 4 GiB + 100 byte value records a length of 100. The bytes are
    // still written, so recovery reads 100 of them and then parses the remainder
    // as the next record — the log is corrupt from that point on, and nothing
    // reports it. Refusing up front turns silent corruption into an error the
    // caller can act on.
    constexpr size_t kMaxFieldBytes = std::numeric_limits<uint32_t>::max();
    if (key.size() > kMaxFieldBytes || value.size() > kMaxFieldBytes) {
        throw std::length_error(
            "WAL: key or value exceeds the 4 GiB limit imposed by the on-disk "
            "length field (key=" + std::to_string(key.size()) +
            ", value=" + std::to_string(value.size()) + ")");
    }

    // Allocate 4096-aligned buffer for O_DIRECT
    size_t size = 1 + 4 + 4 + key.size() + value.size() + 4;
    size_t aligned_size = (size + 511) & ~511; // Align to 512 for O_DIRECT
    
    void* buf;
    if (posix_memalign(&buf, 4096, aligned_size) != 0) throw std::runtime_error("WAL: memalign failed");
    std::memset(buf, 0, aligned_size);

    char* p = static_cast<char*>(buf);
    *p++ = static_cast<uint8_t>(type);
    // Explicit casts: the bounds above guarantee these fit, and stating the
    // conversion stops a future reader wondering whether it was intended.
    uint32_t klen = static_cast<uint32_t>(key.size());
    uint32_t vlen = static_cast<uint32_t>(value.size());
    std::memcpy(p, &klen, 4); p += 4;
    std::memcpy(p, &vlen, 4); p += 4;
    std::memcpy(p, key.data(), klen); p += klen;
    std::memcpy(p, value.data(), vlen); p += vlen;
    
    uint32_t crc = crc32(static_cast<uint8_t>(type), key, value);
    std::memcpy(p, &crc, 4);

    // Prepare io_uring SQE.
    //
    // io_uring_get_sqe() yields nullptr when the submission ring has no free
    // slot. The result was previously passed straight to io_uring_prep_write(),
    // which dereferences it. Reaching that state needs submissions to stop
    // draining the ring — see the unchecked io_uring_submit() below, whose
    // failures leave SQEs queued — but the pointer has to be checked either way.
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Flush what is queued to release slots, harvest completions, retry once.
        int flushed = io_uring_submit(&ring_);
        if (flushed < 0) {
            free(buf);
            throw std::runtime_error(std::string("WAL: io_uring_submit failed while draining: ")
                                     + std::strerror(-flushed));
        }
        if (flushed > 0) {
            pending_cqes_ += static_cast<size_t>(flushed);
        }
        reap();
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            free(buf);
            throw std::runtime_error("WAL: io_uring submission queue full");
        }
    }

    // Carry the requested length alongside the buffer so the completion can be
    // compared against it.
    auto* pending = new PendingWrite{ buf, aligned_size };

    io_uring_prep_write(sqe, fd_, buf, aligned_size, -1); // -1 = current offset (append)
    io_uring_sqe_set_data(sqe, pending);
    
    // io_uring_submit() returns a negative errno on failure. The result was
    // discarded, so a failed submission was indistinguishable from a successful
    // one. Two things follow from that: no completion is ever produced for the
    // write, so the buffer tagged on the SQE is never freed by reap(); and the
    // SQE stays queued, consuming one of the ring's eight slots. Repeated
    // failures therefore leak a buffer each time and eventually exhaust the
    // ring, which is what makes io_uring_get_sqe() start returning nullptr.
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) {
        // Detach the buffer from the completion tag, but deliberately do not
        // free it.
        //
        // A failed submit does not necessarily consume the SQE. It stays in the
        // ring with sqe->addr still pointing at this buffer, and a later
        // io_uring_submit() may carry it to the kernel — which would then DMA
        // out of freed memory and write whatever now occupies it into the log.
        // Clearing user_data only stops reap() from freeing the pointer twice;
        // it does nothing about the address the kernel would read from.
        //
        // The buffer is therefore retained for the lifetime of the ring. That
        // is a leak, and an intentional one: reclaiming it safely requires
        // tearing the ring down so the entry can never be submitted, which is
        // beyond what this change should do to the write path.
        io_uring_sqe_set_data(sqe, nullptr);
        // `pending` is retained for the same reason as the buffer it describes.
        throw std::runtime_error(std::string("WAL: io_uring_submit failed: ")
                                 + std::strerror(-submitted));
    }
    if (submitted > 0) {
        pending_cqes_ += static_cast<size_t>(submitted);
    }

    // Reap any completed CQEs (non-blocking) to free buffers promptly
    reap();
}

void WAL::recordFailure(const std::string& detail) {
    // Keep the first failure: it is the one that explains where the log stopped
    // being trustworthy. Later ones are usually consequences of it.
    if (failure_.empty()) {
        failure_ = detail;
    }
    std::cerr << "[WAL] " << detail << std::endl;
}

void WAL::reap() {
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        auto* pending = static_cast<PendingWrite*>(io_uring_cqe_get_data(cqe));

        // cqe->res carries the write's outcome and was previously discarded, so
        // -ENOSPC, -EIO and partial writes all looked identical to success. A
        // negative value means nothing reached the log; a value below the
        // requested length means the record on disk is truncated. Either way the
        // caller has been told the entry is recoverable when it is not.
        if (pending) {
            if (cqe->res < 0) {
                recordFailure(std::string("write failed: ") + std::strerror(-cqe->res));
            } else if (static_cast<size_t>(cqe->res) < pending->bytes) {
                recordFailure("short write: " + std::to_string(cqe->res) + " of "
                              + std::to_string(pending->bytes) + " bytes reached the log");
            }

            free(pending->buf);
            delete pending;
        }

        io_uring_cqe_seen(&ring_, cqe);
        if (pending_cqes_ > 0) {
            --pending_cqes_;
        }
    }
}

void WAL::drain() {
    reap();
    while (pending_cqes_ > 0 && ring_ready_) {
        struct __kernel_timespec ts;
        ts.tv_sec = 5;
        ts.tv_nsec = 0;

        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);

        if (ret == -EINTR) {
            continue;
        }

        if (ret < 0) {
            recordFailure("drain wait failed or timed out: " + std::string(std::strerror(-ret)));
            break;
        }

        reap();
    }
}

uint32_t WAL::crc32(uint8_t type, const std::string& key, const std::string& value) {
    uint32_t crc = 0xFFFFFFFF;
    auto update = [&](uint8_t byte) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    };
    update(type);
    for (unsigned char c : key) update(c);
    for (unsigned char c : value) update(c);
    return crc ^ 0xFFFFFFFF;
}
