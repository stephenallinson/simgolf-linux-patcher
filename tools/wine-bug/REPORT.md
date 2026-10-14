# Draft WineHQ bug report: mmioAdvance (MMIO_READ) drops bytes the caller hasn't consumed

**Component:** winmm
**Version:** wine-11.19 (the code is the same in current git)
**Application:** Sid Meier's SimGolf (2002). Streamed sounds never end and loop their last buffer forever.

## Summary

When a file is opened for buffered reading and the caller accesses the I/O buffer directly (`mmioGetInfo` / `mmioSetInfo` / `mmioAdvance`), Wine's `mmioAdvance(..., MMIO_READ)` fills the buffer starting at the end of the previous buffer (`lDiskOffset`). It ignores where the caller stopped reading (`pchNext`). Any bytes between `pchNext` and `pchEndRead` are silently skipped.

Applications that advance before consuming the whole buffer rely on the read continuing from `pchNext`. They lose data under Wine.

## Steps to reproduce

The attached `mmioadvance.c` builds with `i686-w64-mingw32-gcc -O2 -o mmioadvance.exe mmioadvance.c -lwinmm`. It:
1. writes a 300-byte file containing bytes 0..255, 0..43;
2. opens it with `MMIO_READ | MMIO_ALLOCBUF` and a 128-byte buffer;
3. calls `mmioGetInfo`, then `mmioAdvance(MMIO_READ)`, which fills bytes 0..127;
4. consumes 100 bytes (`pchNext += 100`) and calls `mmioSetInfo`;
5. calls `mmioAdvance(MMIO_READ)` and prints the next byte.

**Expected:** the next byte is 100 (continuing from `pchNext`, `lBufOffset` 100).

**Wine 11.19:**
```
first buffer: lBufOffset 0, 128 bytes, first byte 0
after advance: lBufOffset 128, next byte 128 (expected 100)
```
Bytes 100..127 are lost.

## Analysis

`mmioAdvance` (dlls/winmm/mmio.c) calls `MMIO_GrabNextBuffer(wm, TRUE)`, which sets `lBufOffset = lDiskOffset` and reads `cchBuffer` bytes from there. `lDiskOffset` is the file position after the previous buffer fill, not `lBufOffset + (pchNext - pchBuffer)`. The caller's `pchNext` is only used in the write path.

A fix for the read path: before grabbing the next buffer, if `pchNext < pchEndRead`, seek the I/O procedure to `lBufOffset + (pchNext - pchBuffer)` (`MMIOM_SEEK`, which also updates `lDiskOffset`). `wm->info.pchNext` is in sync after `mmioSetInfo`.

## Impact (SimGolf)

SimGolf's `sound.dll` streams long WAVs through a 128 KB ring buffer:
- On each stream start it fills the ring from the first 128 KB I/O buffer minus 4 bytes (the ring keeps a 4-byte gap), then calls `mmioAdvance`.
- Under Wine those 4 bytes are skipped, so the byte count never reaches the `data` chunk size.
- The stream is therefore never marked finished, and its mixer replays the ring forever: ambient sounds and voice lines repeat endlessly, even after leaving the course.

On Windows the game doesn't do this. Wrapping `mmioAdvance` to re-read from `pchNext` (seek back to that position, then advance again) fixes it under Wine.

(Not yet verified on real Windows with the test program. Running it there would confirm the expected output.)
