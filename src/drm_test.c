#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

struct modeset_dev;
static int mode_set_open(int *pfd, const char *node);
static int modeset_prepare(int fd);
static int modeset_setup_dev(int fd, struct drm_mode_card_res *res,
                             struct drm_mode_get_connector *conn,
                             struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_dev *dev);
static int modeset_find_crtc(int fd, struct drm_mode_card_res *res,
                             struct drm_mode_get_connector *conn,
                             struct modeset_dev *dev);
static void modeset_draw(void);
static void modeset_cleanup(int fd);


static int mode_set_open(int *pfd, const char *node)
{
    int fd, ret;
    
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s': %m\n", node);
        return ret;
    }

    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

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

struct modeset_dev {
	struct modeset_dev *next;

	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;

	struct drm_mode_modeinfo mode;
	uint32_t fb;
	uint32_t conn;
	uint32_t crtc;
	struct drm_mode_crtc *saved_crtc;
};

static struct modeset_dev *modeset_list = NULL;

static int modeset_prepare(int fd)
{
    struct drm_mode_card_res res = {0};
    int ret;

    ret = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (ret) {
        fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
                errno);
        return -errno;
    }

    if (res.count_fbs) {
        res.fb_id_ptr = (__u64)calloc(sizeof(uint32_t), res.count_fbs);
    }
    if (res.count_crtcs) {
        res.crtc_id_ptr = (__u64)calloc(sizeof(uint32_t), res.count_crtcs);
    }
    if (res.count_connectors) {
        res.connector_id_ptr = (__u64)calloc(sizeof(uint32_t), 
                                             res.count_connectors);
    }
    if (res.count_encoders) {
        res.encoder_id_ptr = (__u64)calloc(sizeof(uint32_t), res.count_encoders);
    }

    ret = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (ret) {
        fprintf(stderr, "cannot retrieve DRM resources\n");
        goto res_free;
    }

    /* printf("crts counts %d\n", res.count_crtcs); */
    /* printf("connector counts %d\n", res.count_connectors); */
    /* printf("fb counts %d\n", res.count_fbs); */
    struct modeset_dev *dev;
    for (int i = 0; i < res.count_connectors; ++i) {
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = ((uint32_t*)res.connector_id_ptr)[i];
        ret = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
        if (ret) {
            fprintf(stderr, "cannot retrieve DRM connector %u : %m",
                    conn.connector_id);
            continue;
        }

        dev = calloc(sizeof(*dev), 1);
        dev->conn = conn.connector_id;

        ret = modeset_setup_dev(fd, &res, &conn, dev);
        if (ret) {
            if (ret != -ENOENT) {
                errno = -ret;
                fprintf(stderr, "cannot setup device for connector %u:%u (%d)"
                        ": %m\n", i, conn.connector_id, errno);
            }
            free(dev);
            continue;
        }
        dev->next = modeset_list;
        modeset_list = dev;
    }

res_free:
    free((void*)res.fb_id_ptr);
    free((void*)res.crtc_id_ptr);
    free((void*)res.connector_id_ptr);
    free((void*)res.encoder_id_ptr);
    return ret;
}

static int modeset_setup_dev(int fd, struct drm_mode_card_res *res,
                             struct drm_mode_get_connector *conn,
                             struct modeset_dev *dev)
{
    int ret;

    if (conn->count_modes == 0) {
        fprintf(stderr, "no valid mode for connector %u\n", 
                conn->connector_id);
        return -EFAULT;
    }

    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u64 encoders_ptr;
    struct drm_mode_modeinfo *modeinfo = NULL;

    if (conn->count_props) {
        props_ptr = (__u64)calloc(sizeof(uint32_t), conn->count_props);
        if (!props_ptr)
            goto err_alloc;

        prop_values_ptr = (__u64)calloc(sizeof(uint32_t), conn->count_props);
        if (!prop_values_ptr)
            goto err_alloc;
    }
    if (conn->count_encoders) {
        encoders_ptr = (__u64)calloc(sizeof(uint32_t), conn->count_encoders);
        if (!encoders_ptr)
            goto err_alloc;
    }
    if (conn->count_modes) {
        modeinfo = 
            calloc(sizeof(struct drm_mode_modeinfo), conn->count_modes);
        if (!modeinfo)
            goto err_alloc;
    }

    conn->props_ptr = props_ptr;
    conn->prop_values_ptr = prop_values_ptr;
    conn->modes_ptr = (__u64)modeinfo;
    conn->encoders_ptr = encoders_ptr;
    
    ret = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn);
    if (ret) {
        fprintf(stderr, "cannot retrieve connector: %d: %m\n", 
                conn->connector_id);
        goto err_alloc;
    }

    memcpy(&dev->mode, (struct drm_mode_modeinfo *)conn->modes_ptr,
            sizeof(dev->mode));
    dev->width = modeinfo[0].hdisplay;
    dev->height = modeinfo[0].vdisplay;
    printf("mode for connector %u is %ux%u\n", conn->connector_id,
            dev->width, dev->height);

    ret = modeset_find_crtc(fd, res, conn, dev);
    if (ret) {
        goto err_alloc;
    }

    ret = modeset_create_fb(fd, dev);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer to connector %u\n",
                conn->connector_id);
        goto err_alloc;
    }

err_alloc:
    free((void*)conn->props_ptr);
    free((void*)conn->prop_values_ptr);
    free((void*)conn->modes_ptr);
    free((void*)conn->encoders_ptr);

    return ret;
}

static int modeset_find_crtc(int fd, struct drm_mode_card_res *res,
                             struct drm_mode_get_connector *conn,
                             struct modeset_dev *dev)
{
    int ret = -1;
    struct drm_mode_get_encoder enc = {0};
    uint32_t crtc_id;
    struct modeset_dev *iter;

    if (conn->encoder_id) {
        enc.encoder_id = conn->encoder_id;
        ret = ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc);
    } 

    if (ret) {
        for (int i = 0; i < conn->count_encoders; ++i) {
            memset(&enc, 0, sizeof(enc));
            uint32_t *pencoders = (uint32_t*)conn->encoders_ptr;
            enc.encoder_id = pencoders[i];
            int ret1 = ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc);
            if (ret1) {
                fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
                        i, pencoders[i], errno);
                continue;
            }

            for (uint j = 0; j < res->count_crtcs; ++j) {
                if (!(enc.possible_crtcs & (1 << j)))
                    continue;
                
                crtc_id = ((uint32_t*)res->crtc_id_ptr)[i];
                for (iter = modeset_list; iter; iter = iter->next) {
                    if (iter->crtc == crtc_id) {
                        crtc_id = -1;
                        break;
                    }
                }
                
                if(crtc_id >= 0) {
                    dev->crtc = crtc_id;
                    return 0;
                }
            }
        }
    } else {
        if (enc.crtc_id) {
            crtc_id = enc.crtc_id;
            for (iter = modeset_list; iter; iter = iter->next) {
                if (iter->crtc == crtc_id) {
                    crtc_id = -1;
                    break;
                }
            }

            if (crtc_id >= 0) {
                dev->crtc = crtc_id;
                return 0;
            }
        } 
    }

    fprintf(stderr, "cannot find a suitalble CRTC for connector %u\n", 
            conn->connector_id);
    return -ENOENT;
}

static int modeset_create_fb(int fd, struct modeset_dev *dev)
{
    struct drm_mode_create_dumb creq = {0};
    struct drm_mode_destroy_dumb dreq = {0};
    struct drm_mode_map_dumb mreq = {0};
    int ret;

    creq.width = dev->width;
    creq.height = dev->height;
    creq.bpp = 32;
    ret = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    dev->stride = creq.pitch;
    dev->size = creq.size;
    dev->handle = creq.handle;

    struct drm_mode_fb_cmd fb_cmd = {0};
    fb_cmd.width = creq.width;
    fb_cmd.height = creq.height;
    fb_cmd.depth = 24;
    fb_cmd.pitch = creq.pitch;
    fb_cmd.handle = creq.handle;
    fb_cmd.bpp = creq.bpp;
    ret = ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd); 
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    mreq.handle = dev->handle;
    ret = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    dev->map = mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 
                    mreq.offset);
    if (dev->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }
    memset(dev->map, 0, dev->size);

    return 0;

err_fb:
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &dev->fb);
err_destroy:
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = dev->handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

int main()
{
    int ret, fd;
    
    ret = mode_set_open(&fd, "/dev/dri/card0");
    if (ret) {
        printf("opened failed");
        goto error_return;
    }

    ret = modeset_prepare(fd);
    if (ret) {
        goto error_close_return;
    }
    
    struct modeset_dev *iter;
    for (iter = modeset_list; iter; iter = iter->next) {
        iter->saved_crtc = calloc(sizeof(struct drm_mode_crtc), 1);
        iter->saved_crtc->crtc_id = iter->crtc;
        ioctl(fd, DRM_IOCTL_MODE_GETCRTC, iter->saved_crtc);
        
        struct drm_mode_crtc crtc = {0};
        crtc.crtc_id = iter->crtc;
        crtc.fb_id = iter->fb;
        crtc.set_connectors_ptr = (__u64)iter->conn;
        crtc.x = 0;
        crtc.y = 0;
        memcpy(&crtc.mode, &iter->mode, sizeof(crtc.mode));
        ret = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
        if (ret) {
            fprintf(stderr, "cannot set CRTC for connector %u (%d) : %m\n",
                    iter->conn, errno);
        }
    }

    modeset_draw();

    modeset_cleanup(fd);

    ret = 0;

error_close_return:
    close(fd);

error_return:
    if (ret) {
        errno = -ret;
        fprintf(stderr, "modeset failed with error %d: %m\n", errno);
    }
    return ret;
}

static uint8_t next_color(uint *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

static void modeset_draw(void)
{
    uint8_t r, g, b;
    uint r_up, g_up, b_up;
    uint i, j, k, off;
	struct modeset_dev *iter;

	srand(time(NULL));
	r = rand() % 0xff;
	g = rand() % 0xff;
	b = rand() % 0xff;
	r_up = g_up = b_up = 1;

	for (i = 0; i < 50; ++i) {
		r = next_color(&r_up, r, 20);
		g = next_color(&g_up, g, 10);
		b = next_color(&b_up, b, 5);

		for (iter = modeset_list; iter; iter = iter->next) {
			for (j = 0; j < iter->height; ++j) {
				for (k = 0; k < iter->width; ++k) {
					off = iter->stride * j + k * 4;
					*(uint32_t*)&iter->map[off] =
						     (r << 16) | (g << 8) | b;
				}
			}
		}

		usleep(100000);
	}
}

static void modeset_cleanup(int fd)
{
    struct modeset_dev *iter;
	struct drm_mode_destroy_dumb dreq;

	while (modeset_list) {
        iter = modeset_list;
        modeset_list = iter->next;
        
        ioctl(fd, DRM_IOCTL_MODE_SETCRTC, iter->saved_crtc);
        free(iter->saved_crtc);

        munmap(iter->map, iter->size);

        ioctl(fd, DRM_IOCTL_MODE_RMFB, iter->fb);
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = iter->handle;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

        free(iter);
    }
}

