// Lists which KGSL ioctl numbers the security policy lets this process issue. SELinux filters
// ioctls by number before the driver sees them and answers EACCES; anything else (EINVAL, EFAULT,
// ENOTTY...) means the call got through to the driver.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
    const int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open /dev/kgsl-3d0: %s\n", strerror(errno));
        return 1;
    }
    printf("allowed:");
    int denied = 0;
    for (unsigned nr = 0; nr < 0x80; ++nr) {
        errno = 0;
        // A null argument can never succeed, so nothing is changed by asking.
        const int result = ioctl(fd, _IOC(_IOC_READ | _IOC_WRITE, 0x09, nr, 0), NULL);
        if (result != 0 && errno == EACCES) {
            ++denied;
            continue;
        }
        printf(" %#x(%s)", nr, result == 0 ? "ok" : strerror(errno));
    }
    printf("\ndenied: %d of 128\n", denied);
    close(fd);
    return 0;
}
