// The POSIX file calls the importer makes, over Win32, for the Windows build. Built with
// _CRT_DECLARE_NONSTDC_NAMES=0 (tools/importer/CMakeLists.txt), so the C runtime's own open/read/stat do not clash.
// Paths are UTF-8; they become absolute UTF-16 paths with the \\?\ prefix, so the RomFS's ~103k files can sit
// under a deep folder or a user name with accents (G8). Offsets and sizes are 64-bit (the NSPs are 4-7 GB).
#pragma once
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

using ssize_t = SSIZE_T;
using off_t = int64_t;

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_CLOEXEC 0
#define S_IFREG 0x8000
#define S_ISREG(m) (((m) & 0xF000) == S_IFREG)

namespace nxwin {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// "C:/a/b" or "a/b" -> "\\?\C:\a\b" (the prefix turns off the 260-character limit and wants backslashes).
inline std::wstring long_path(const char* path) {
    std::wstring w = widen(path);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!n) return w;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    full.resize(n);
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

inline int set_errno_from_win() {
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: errno = EEXIST; break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: errno = EACCES; break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
        default: errno = EIO; break;
    }
    return -1;
}

}  // namespace nxwin

inline int open(const char* path, int flags, int = 0) {
    std::wstring w = nxwin::long_path(path);
    DWORD access = (flags & _O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
    DWORD create = (flags & _O_CREAT) ? ((flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING;
    HANDLE h = CreateFileW(w.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE | (access == GENERIC_READ ? FILE_SHARE_WRITE : 0),
                           nullptr, create, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nxwin::set_errno_from_win();
    int fd = _open_osfhandle(intptr_t(h), (flags & _O_WRONLY) ? _O_WRONLY | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (fd < 0) CloseHandle(h);
    return fd;
}

inline int close(int fd) { return _close(fd); }

// pread / write / read through the handle: 64-bit offsets, and pread leaves no shared file position behind.
inline ssize_t pread(int fd, void* buf, size_t n, uint64_t off) {
    HANDLE h = HANDLE(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = DWORD(off);
    ov.OffsetHigh = DWORD(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, DWORD(n > 0x40000000 ? 0x40000000 : n), &got, &ov))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : nxwin::set_errno_from_win();
    return ssize_t(got);
}

inline ssize_t write(int fd, const void* buf, size_t n) {
    DWORD put = 0;
    if (!WriteFile(HANDLE(_get_osfhandle(fd)), buf, DWORD(n > 0x40000000 ? 0x40000000 : n), &put, nullptr))
        return nxwin::set_errno_from_win();
    return ssize_t(put);
}

inline ssize_t read(int fd, void* buf, size_t n) {
    DWORD got = 0;
    if (!ReadFile(HANDLE(_get_osfhandle(fd)), buf, DWORD(n > 0x40000000 ? 0x40000000 : n), &got, nullptr))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : nxwin::set_errno_from_win();
    return ssize_t(got);
}

inline int fdatasync(int fd) { return FlushFileBuffers(HANDLE(_get_osfhandle(fd))) ? 0 : nxwin::set_errno_from_win(); }

inline int mkdir(const char* path, int) {
    return CreateDirectoryW(nxwin::long_path(path).c_str(), nullptr) ? 0 : nxwin::set_errno_from_win();
}

inline int unlink(const char* path) { return DeleteFileW(nxwin::long_path(path).c_str()) ? 0 : nxwin::set_errno_from_win(); }

// POSIX rename replaces the target; MoveFileEx does only when asked. A macro, as <cstdio> declares rename (and
// fopen below): include this header after the standard ones.
inline int nx_rename(const char* from, const char* to) {
    return MoveFileExW(nxwin::long_path(from).c_str(), nxwin::long_path(to).c_str(), MOVEFILE_REPLACE_EXISTING) ? 0
                                                                                                                 : nxwin::set_errno_from_win();
}
#define rename nx_rename

struct stat {
    uint64_t st_size = 0;
    int st_mode = 0;
};

inline int stat(const char* path, struct stat* st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(nxwin::long_path(path).c_str(), GetFileExInfoStandard, &a)) return nxwin::set_errno_from_win();
    st->st_size = uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
    st->st_mode = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x4000 : S_IFREG;
    return 0;
}

inline int fstat(int fd, struct stat* st) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(HANDLE(_get_osfhandle(fd)), &sz)) return nxwin::set_errno_from_win();
    st->st_size = uint64_t(sz.QuadPart);
    st->st_mode = S_IFREG;
    return 0;
}

struct statvfs {
    uint64_t f_bavail = 0, f_frsize = 1;
};

inline int statvfs(const char* path, struct statvfs* vs) {
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(nxwin::long_path(path).c_str(), &avail, nullptr, nullptr)) return nxwin::set_errno_from_win();
    vs->f_bavail = avail.QuadPart;
    vs->f_frsize = 1;
    return 0;
}

inline FILE* nx_fopen(const char* path, const char* mode) {
    return _wfopen(nxwin::long_path(path).c_str(), nxwin::widen(mode).c_str());
}
#define fopen nx_fopen

#endif  // _WIN32
