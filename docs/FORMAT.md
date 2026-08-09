# On-disk formats

Two file types: the write-ahead log and the SSTable. Both carry a magic number
and a format version as of version 2.

## Byte order — a known limitation

**Every integer in both formats is written in host-native byte order.** A file
written on a little-endian machine cannot be read on a big-endian one, and the
reader will not detect the mismatch: it will parse the bytes in the wrong order
and produce plausible-looking nonsense.

This is deliberate for now, not an oversight. It was raised as
[#50](https://github.com/harsharajkumar-273/lsm_tree/issues/50) and the decision
was to add format identification first and leave the byte order alone:

- nothing currently moves these files between machines — they are not a wire
  format, and no issue or requirement asks for cross-architecture portability
- the project targets Linux on x86-64 with `io_uring` and NVMe
- changing the byte order without a version field would silently corrupt every
  existing file, which is the problem the version field exists to prevent

**Cross-architecture portability is not currently guaranteed.** If it becomes a
requirement, the change is now safe to make: bump the version, byte-swap through
`htole32`/`le32toh` from `<endian.h>`, and have the reader dispatch on the
version it finds. On little-endian hosts those helpers compile to nothing, so the
cost is code surface rather than throughput.

## Versions

| Version | Identification | Notes |
| ------- | -------------- | ----- |
| 1 | none — no magic anywhere | the historical format |
| 2 | magic + version field | identical to v1 apart from the identification |

Readers accept both. Writers only emit v2, so existing files keep working and
compaction rewrites v1 SSTables into v2 over time without a migration step.

An unrecognised version is refused rather than guessed at, and so is a file whose
header is truncated — a malformed header must not fall through into record
parsing.

## WAL

```
offset 0     [magic u32 = "LSMW"][version u32][zero padding to 512]
offset 512   record
offset 1024  record
...
```

The header occupies a full 512-byte block, for two reasons:

- the log is opened `O_DIRECT`, which requires the offset, length and buffer
  address of every write to be sector-aligned. A 16-byte header cannot be
  written at all.
- a whole block keeps every record on the same 512-byte grid measured from the
  start of the file, which is what recovery's resynchronisation arithmetic
  (`((entry_start / 512) + 1) * 512`) assumes. A partial-block header would
  have shifted every record off that grid.

Only the first 8 bytes are used. The rest is zero.

### Record

```
[type u8][key_len u32][value_len u32][key bytes][value bytes][crc32 u32]
```

Padded to a 512-byte boundary. `type` is `0x01` PUT or `0x02` DEL; a `0x00` byte
marks padding, and recovery skips to the next boundary on seeing one.

A v1 log begins directly with a record, whose first byte is a type tag. It can
never be mistaken for a v2 header, because no record starts with `LSMW`.

## SSTable

```
[data blocks][index][bloom filter]
[index_offset u64][bloom_offset u64][magic u32 = "LSMT"][version u32]
```

The magic and version are **appended** to the trailer rather than prepended. The
reader locates the offsets by seeking back from the end of the file, so appending
keeps that seek at a fixed distance for each version and lets a v1 file be
recognised by simply not having a magic where a v2 file does. Prepending would
have required knowing the version before being able to find it.

A v2 trailer is 24 bytes; a v1 trailer is 16.

### Detection

Read the last 8 bytes. `LSMT` present means v2 and the trailer starts 24 bytes
from the end; absent means v1 and it starts at 16.

A v1 file cannot produce a false positive: those 8 bytes would be the tail of
`bloom_offset`, a file position, which would have to coincidentally equal the
magic in its low word and a valid version in its high word.

## Adding a new version

1. Add the constant to `src/format.h`.
2. Teach both readers to dispatch on it; leave the v1 and v2 paths alone.
3. Write the new version only — existing files stay readable.
4. Add cases to `tests/test_format_version.cpp` covering detection of the new
   version, and rejection of an unknown one.

The point of the version field is that step 3 is safe. Before it existed, any
format change made old files parse as garbage rather than be rejected.
