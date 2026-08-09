#pragma once

#include <cstdint>

// On-disk format identification.
//
// Neither the WAL nor the SSTable carried a magic number or a version field, so
// a reader had no way to tell one format from another. Any future change to
// either layout would make existing files parse as garbage rather than be
// rejected — recovery would misinterpret them, not refuse them.
//
// This adds that identification and nothing else. Integers remain in host-native
// byte order exactly as before; see docs/FORMAT.md for what that means and why
// it is deliberate for now.
//
// Version 1 is the historical format, which has no header at all and is
// identified by the *absence* of these magic values. Version 2 is identical on
// the wire apart from the identification described below.

namespace lsm::format {

// "LSMW" and "LSMT", little-endian on the platforms this targets. The two differ
// so a WAL cannot be opened as an SSTable or the reverse.
inline constexpr uint32_t kWalMagic     = 0x574D534CU;  // 'L','S','M','W'
inline constexpr uint32_t kSSTableMagic = 0x544D534CU;  // 'L','S','M','T'

inline constexpr uint32_t kVersion1 = 1;   // historical, unidentified
inline constexpr uint32_t kVersion2 = 2;   // magic + version present

inline constexpr uint32_t kCurrentVersion = kVersion2;

// The WAL header occupies one full 512-byte block.
//
// The design proposal on #50 said 16 bytes. That is not writable here: the log
// is opened O_DIRECT, which requires the offset, the length and the buffer
// address of every write to be sector-aligned. A 16-byte write fails with
// EINVAL before it reaches the disk.
//
// A full block also keeps every record aligned to a 512-byte boundary measured
// from the start of the file, which is what recovery's resynchronisation
// arithmetic assumes -- `((entry_start / 512) + 1) * 512`. A 16-byte header
// would have shifted every record off that grid and broken the block-skip logic
// added for #29. Only the first 8 bytes are used; the rest is zero padding.
inline constexpr uint64_t kWalHeaderBytes = 512;

// The SSTable trailer grows from 16 bytes to 24: the two existing offsets, then
// the magic and version. Appending rather than prepending means the reader still
// finds the offsets by seeking from the end, and a v1 file simply has no magic
// where a v2 file does.
inline constexpr uint64_t kSSTableFooterV1Bytes = 16;
inline constexpr uint64_t kSSTableFooterV2Bytes = 24;

}  // namespace lsm::format
