// Diagnoses why a Vulkan driver cannot allocate GPU memory on a Qualcomm (KGSL) device.
// It repeats the allocation Turnip does first, optionally after reserving the address range the
// emulator reserves for its guest, and prints the error of each step.
//   kgsl_probe [reserve]
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KGSL_IOC_TYPE 0x09

struct kgsl_gpumem_alloc_id {
    unsigned int id;
    unsigned int flags;
    size_t size;
    size_t mmapsize;
    unsigned long gpuaddr;
    unsigned long pad[2];
};
#define IOCTL_KGSL_GPUMEM_ALLOC_ID _IOWR(KGSL_IOC_TYPE, 0x34, struct kgsl_gpumem_alloc_id)

struct kgsl_gpuobj_alloc {
    uint64_t size;
    uint64_t flags;
    uint64_t va_len;
    uint64_t mmapsize;
    unsigned int id;
    unsigned int metadata_len;
    uint64_t metadata;
};
#define IOCTL_KGSL_GPUOBJ_ALLOC _IOWR(KGSL_IOC_TYPE, 0x45, struct kgsl_gpuobj_alloc)

struct kgsl_gpuobj_info {
    uint64_t gpuaddr;
    uint64_t flags;
    uint64_t size;
    uint64_t va_len;
    uint64_t va_addr;
    unsigned int id;
};
#define IOCTL_KGSL_GPUOBJ_INFO _IOWR(KGSL_IOC_TYPE, 0x47, struct kgsl_gpuobj_info)

#define KGSL_MEMFLAGS_USE_CPU_MAP 0x10000000u
#define KGSL_CACHEMODE_WRITECOMBINE (0u << 26)
#define KGSL_CACHEMODE_WRITEBACK (3u << 26)

static void try_alloc(int fd, const char* label, unsigned int flags, size_t size) {
    struct kgsl_gpumem_alloc_id request;
    memset(&request, 0, sizeof(request));
    request.flags = flags;
    request.size = size;
    if (ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &request) != 0) {
        printf("%-28s GPUMEM_ALLOC_ID failed: %s\n", label, strerror(errno));
        return;
    }
    void* map = mmap(NULL, request.mmapsize ? request.mmapsize : size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)request.id << 12);
    struct kgsl_gpuobj_info info;
    memset(&info, 0, sizeof(info));
    info.id = request.id;
    const int info_result = ioctl(fd, IOCTL_KGSL_GPUOBJ_INFO, &info);
    printf("%-28s id=%u gpuaddr=%#lx (info %d: %#llx) mmap=%p%s%s\n", label, request.id,
           request.gpuaddr, info_result, (unsigned long long)info.gpuaddr,
           map == MAP_FAILED ? NULL : map, map == MAP_FAILED ? " mmap failed: " : "",
           map == MAP_FAILED ? strerror(errno) : "");
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "reserve") == 0) {
        // What shadPS4 keeps for the guest on a 39-bit host: 0x400000 up to 256 GB.
        void* reserved = mmap((void*)0x400000, 0x4000000000ull - 0x400000, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                              -1, 0);
        printf("guest reservation: %p%s%s\n", reserved == MAP_FAILED ? NULL : reserved,
               reserved == MAP_FAILED ? " failed: " : "",
               reserved == MAP_FAILED ? strerror(errno) : "");
    }

    const int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open /dev/kgsl-3d0: %s\n", strerror(errno));
        return 1;
    }
    try_alloc(fd, "writecombine 64K", KGSL_CACHEMODE_WRITECOMBINE, 0x10000);
    try_alloc(fd, "writeback 64K", KGSL_CACHEMODE_WRITEBACK, 0x10000);
    try_alloc(fd, "writecombine+cpu_map 64K", KGSL_CACHEMODE_WRITECOMBINE | KGSL_MEMFLAGS_USE_CPU_MAP,
              0x10000);
    try_alloc(fd, "writecombine 16M", KGSL_CACHEMODE_WRITECOMBINE, 16u << 20);
    close(fd);
    return 0;
}
