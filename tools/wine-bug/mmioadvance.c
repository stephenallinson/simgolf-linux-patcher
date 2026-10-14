/* Minimal test: does mmioAdvance (MMIO_READ) continue from the caller's
 * pchNext, or skip the bytes the caller hasn't consumed?
 * Build: i686-w64-mingw32-gcc -O2 -o mmioadvance.exe mmioadvance.c -lwinmm
 * Expected (Windows behaviour relied on by e.g. SimGolf's sound.dll):
 *   after advancing with 28 of 128 bytes unconsumed, the next byte is 100. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

int main(void)
{
    char name[] = "mmioadv.bin";
    unsigned char data[300];
    MMIOINFO info = {0};
    HMMIO h;
    FILE *f;
    int i;

    for (i = 0; i < 300; i++) data[i] = (unsigned char)i;
    f = fopen(name, "wb"); fwrite(data, 1, sizeof(data), f); fclose(f);

    info.cchBuffer = 128;                       /* mmio allocates a 128-byte buffer */
    h = mmioOpenA(name, &info, MMIO_READ | MMIO_ALLOCBUF);
    if (!h) { printf("mmioOpen failed\n"); return 1; }
    mmioGetInfo(h, &info, 0);
    mmioAdvance(h, &info, MMIO_READ);           /* fill: file bytes 0..127 */
    printf("first buffer: lBufOffset %ld, %d bytes, first byte %d\n",
           info.lBufOffset, (int)(info.pchEndRead - info.pchBuffer), (unsigned char)info.pchNext[0]);
    info.pchNext += 100;                        /* consume 100 bytes, leave 28 */
    mmioSetInfo(h, &info, 0);
    mmioAdvance(h, &info, MMIO_READ);
    printf("after advance: lBufOffset %ld, next byte %d (expected 100)\n",
           info.lBufOffset, (unsigned char)info.pchNext[0]);
    mmioClose(h, 0);
    DeleteFileA(name);
    return (unsigned char)info.pchNext[0] == 100 ? 0 : 2;
}
