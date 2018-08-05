#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static int modeSetOpen(int *pfd, const char *node)
{
    int fd, ret;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s': %m\n", node);
        return ret;
    }

    struct drm_get_cap cap = {0};

    cap.capability = DRM_CAP_DUMB_BUFFER;
    ret = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    if (ret < 0 || cap.value == 0) {
        fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
                node);
        close(fd);
        return -EOPNOTSUPP;
    }

    *pfd = fd;
    return 0;
}

int main()
{
    int fd;
    
    if (modeSetOpen(&fd, "/dev/dri/card0")) {
        printf("opened failed");
    } else {
        printf("opened success");
        close(fd);
    }

    
    return 0;
}
