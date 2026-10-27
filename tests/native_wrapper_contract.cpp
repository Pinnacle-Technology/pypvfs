// Exercise the actual wrapper with instrumented core calls. This checks the
// commit flag and error propagation without relying on physical disk timing.
#include "pvfs.h"
#include <cstddef>

namespace pvfs {
bool observedCommit = false;
int flushCalls = 0;
std::int32_t captureFlush(std::shared_ptr<PvfsFileHandle>&, bool commit = false) {
    observedCommit = commit;
    ++flushCalls;
    return -123;
}
std::int32_t captureExtract(std::shared_ptr<PvfsFile>&, const char*, const char*) {
    return -124;
}
std::int32_t captureOverwrite(std::shared_ptr<PvfsFile>&, const char*, const char*) {
    return -125;
}
}

#define PVFS_flush captureFlush
#define PVFS_extract captureExtract
#define PVFS_overwrite captureOverwrite
#include "pvfs_wrapper.cpp"
#undef PVFS_flush
#undef PVFS_extract
#undef PVFS_overwrite

static_assert(sizeof(PvfsIndexHeaderWrapper) == 56, "ctypes header ABI changed");
static_assert(offsetof(PvfsIndexHeaderWrapper, startTime) == 24, "startTime ABI changed");
static_assert(offsetof(PvfsIndexHeaderWrapper, endTime) == 40, "endTime ABI changed");

int main() {
    PvfsFileHandleWrapper handle{std::make_shared<pvfs::PvfsFileHandle>()};
    PvfsFileWrapper vfs{std::make_shared<pvfs::PvfsFile>()};
    if (pvfs_flush(&handle, 0) != -123 || pvfs::observedCommit) return 1;
    if (pvfs_flush(&handle, 1) != -123 || !pvfs::observedCommit) return 2;
    if (pvfs_flush(&handle, -1) != -123 || !pvfs::observedCommit) return 3;
    if (pvfs_flush(nullptr, 1) != pvfs::PVFS_ARG_NULL || pvfs::flushCalls != 3) return 4;
    if (extract(&vfs, "input", "output") != -124) return 5;
    if (pvfs_overwrite(&vfs, "entry", "input") != -125) return 6;
    return 0;
}
