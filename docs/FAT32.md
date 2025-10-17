# FAT32 Driver Plan (Draft)

## Initial Goals
- Implement **read-only** FAT32 support to mount a volume and read files/directories.
- Integrate with existing VFS layer (virtual inodes, readdir, file read).

## Required Components
1. **Abstract Block Device**: `int blk_read(uint64_t lba, void* buf, size_t sectors)` initially backed by a RAM buffer.
2. **BPB Parser (BIOS Parameter Block)**: Extract key values:
  - Bytes per sector
  - Sectors per cluster
  - Number of FATs
  - Reserved sectors
  - Sectors per FAT
  - First root cluster (FAT32 uses a root cluster instead of fixed root directory)
3. **FAT Table Handling**: Lazy loading of FAT entries to follow a file's cluster chain.
4. **Directory Entries** (short name + LFN): Ignore LFN initially, use only 8.3; later add LFN parsing.
5. **Cluster Caching**: Temporary buffer for current cluster (simple optimization; no advanced LRU initially).
6. **Path → Cluster Translation**: Walk directory components using case-insensitive 8.3 comparison.

## VFS Mapping
- Each file/directory inode contains:
  - First cluster
  - Size (for files)
  - Directory flag
- `lookup(path)`: resolves starting from the root cluster.
- `readdir(path)`: lists entries in the directory's cluster chain.
- `read(file, offset, buf, len)`: follows FAT chain until the cluster containing the offset.

## Initial Version Limits
- No write/modify/extended metadata.
- Ignore advanced attributes (only DIR/FILE, hidden ignored).
- No timestamps.
- No LFN support in first iteration.

## Future Extensions
- LFN support (sequence of entries with attribute 0x0F).
- Write support (allocate free clusters, update FAT, create entries).
- Multi-cluster cache + prefetch.
- Attribute handling (RO, H, S, A).
- Timestamp (DOS Date) conversion.

## exFAT Considerations
exFAT differs significantly (allocation bitmap, variable directory entries, upcase table). Will be addressed after FAT32 base stabilizes.

## Implementation Sequence
1. Stub block device (dummy buffer with FAT32 image loaded statically).
2. Read BPB and validate signature.
3. Load first root cluster and list via readdir.
4. Implement component lookup.
5. Implement linearized file read.
6. Integrate with VFS mount (e.g. `vfs_mount_fat32(image_base)`).
7. Tests: list root, cat a known file.

## Planned Tests
- Boot kernel with embedded FAT32 image and compare `vls /` output vs expected entries.
- `vcat /README.TXT` matches known content.

## Performance Notes
Initial version sacrifices performance for simplicity; linear cluster traversal per read.

---
Draft – subject to change based on needs.
