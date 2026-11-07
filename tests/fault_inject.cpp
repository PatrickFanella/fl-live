// Test-only LD_PRELOAD fault injector for save boundaries. It affects only
// regular files under $FL_LIVE_FAULT_DIR, and only while $FL_LIVE_FAULT names
// an operation, so stdio, Qt event descriptors and other files are untouched.
//   write  - allow 512 bytes per descriptor, then fail with ENOSPC
//   slow   - every write sleeps 100 ms, then succeeds (a disk that falls behind)
//   sync   - fsync/fdatasync fail with EIO
//   rename - publishing into the directory fails with EIO: rename/renameat/
//            renameat2 (overwrite) and linkat (Qt's O_TMPFILE path for new files)
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool active(const char* operation) {
    const char* value = std::getenv("FL_LIVE_FAULT");
    return value && std::strcmp(value, operation) == 0;
}
bool inside(const char* path) {
    const char* dir = std::getenv("FL_LIVE_FAULT_DIR");
    if (!dir || !path) return false;
    const auto length = std::strlen(dir);
    return std::strncmp(path, dir, length) == 0 && path[length] == '/';
}
bool targeted(int fd) {
    struct stat info {};
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    std::array<char, 64> link{};
    std::array<char, PATH_MAX + 1> path{};
    std::snprintf(link.data(), link.size(), "/proc/self/fd/%d", fd);
    const auto size = readlink(link.data(), path.data(), PATH_MAX);
    if (size <= 0) return false;
    path[static_cast<std::size_t>(size)] = '\0';
    return inside(path.data());
}
template<class F> F real(const char* name) { return reinterpret_cast<F>(dlsym(RTLD_NEXT, name)); }
std::array<std::size_t, 4096> written{};
}

extern "C" {
ssize_t write(int fd, const void* buffer, size_t count) {
    static const auto next = real<ssize_t (*)(int, const void*, size_t)>("write");
    if (active("write") && targeted(fd)) {
        auto& used = written[static_cast<std::size_t>(fd) % written.size()];
        if (used >= 512) { errno = ENOSPC; return -1; }
        count = count < 512 - used ? count : 512 - used;
        const auto result = next(fd, buffer, count);
        if (result > 0) used += static_cast<std::size_t>(result);
        return result;
    }
    if (active("slow") && targeted(fd)) usleep(100000);
    if (fd >= 0) written[static_cast<std::size_t>(fd) % written.size()] = 0;
    return next(fd, buffer, count);
}
int fsync(int fd) {
    static const auto next = real<int (*)(int)>("fsync");
    if (active("sync") && targeted(fd)) { errno = EIO; return -1; }
    return next(fd);
}
int fdatasync(int fd) {
    static const auto next = real<int (*)(int)>("fdatasync");
    if (active("sync") && targeted(fd)) { errno = EIO; return -1; }
    return next(fd);
}
int linkat(int from_dir, const char* from, int to_dir, const char* to, int flags) {
    static const auto next = real<int (*)(int, const char*, int, const char*, int)>("linkat");
    if (active("rename") && inside(to)) { errno = EIO; return -1; }
    return next(from_dir, from, to_dir, to, flags);
}
int rename(const char* from, const char* to) {
    static const auto next = real<int (*)(const char*, const char*)>("rename");
    if (active("rename") && inside(to)) { errno = EIO; return -1; }
    return next(from, to);
}
int renameat(int from_dir, const char* from, int to_dir, const char* to) {
    static const auto next = real<int (*)(int, const char*, int, const char*)>("renameat");
    if (active("rename") && inside(to)) { errno = EIO; return -1; }
    return next(from_dir, from, to_dir, to);
}
int renameat2(int from_dir, const char* from, int to_dir, const char* to, unsigned int flags) {
    static const auto next = real<int (*)(int, const char*, int, const char*, unsigned int)>("renameat2");
    if (active("rename") && inside(to)) { errno = EIO; return -1; }
    return next(from_dir, from, to_dir, to, flags);
}
}
