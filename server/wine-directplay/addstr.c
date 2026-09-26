#include <windows.h>
#include <stdio.h>
/* addstr.exe <dll> : adds an RT_STRING table (ids 0..319) so LoadStringW succeeds without the .mui file. */
int wmain(int argc, wchar_t** argv) {
    HANDLE h = BeginUpdateResourceW(argv[1], FALSE);
    if (!h) { printf("Begin failed %lu\n", GetLastError()); return 1; }
    for (int block = 1; block <= 20; ++block) {
        BYTE buf[4096]; int off = 0;
        for (int i = 0; i < 16; ++i) {
            int id = (block - 1) * 16 + i;
            wchar_t s[64]; int n = swprintf(s, 64, L"DirectPlay8 %d", id);
            WORD len = (WORD)n; memcpy(buf + off, &len, 2); off += 2;
            memcpy(buf + off, s, n * 2); off += n * 2;
        }
        if (!UpdateResourceW(h, RT_STRING, MAKEINTRESOURCEW(block), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), buf, off)) { printf("Update failed %lu\n", GetLastError()); return 1; }
        UpdateResourceW(h, RT_STRING, MAKEINTRESOURCEW(block), 0x409, buf, off);
    }
    if (!EndUpdateResourceW(h, FALSE)) { printf("End failed %lu\n", GetLastError()); return 1; }
    printf("ok\n"); return 0;
}
