#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#include "render.h"

#define DRM_DEVICE "/dev/dri/card0"
#define OUTPUT_WIDTH 720
#define OUTPUT_HEIGHT 576

typedef struct drm_buffer {
    uint32_t handle;
    uint32_t framebuffer;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
} drm_buffer;


typedef struct render_shared {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeCrtc *saved_crtc;
    drmModeModeInfo mode;
    drm_buffer buffer[2];

    pthread_t thread;
    volatile int done;

    uint8_t *image;
    int width;
    int height;
    DrawFunc draw_func;
    int delay;
    uint32_t white;
} render_shared;


static void die(const char *message)
{
    perror(message);
    abort();
}


static void check(int result, const char *message)
{
    if (result < 0) die(message);
}


static void create_buffer(render_shared *r, drm_buffer *buffer)
{
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};

    create.width = OUTPUT_WIDTH;
    create.height = OUTPUT_HEIGHT;
    create.bpp = 32;
    check(drmIoctl(r->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create), "DRM_IOCTL_MODE_CREATE_DUMB");

    buffer->handle = create.handle;
    buffer->pitch = create.pitch;
    buffer->size = create.size;
    handles[0] = buffer->handle;
    pitches[0] = buffer->pitch;
    check(drmModeAddFB2(r->fd, OUTPUT_WIDTH, OUTPUT_HEIGHT,
                        DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                        &buffer->framebuffer, 0),
          "drmModeAddFB2");

    map.handle = buffer->handle;
    check(drmIoctl(r->fd, DRM_IOCTL_MODE_MAP_DUMB, &map),
          "DRM_IOCTL_MODE_MAP_DUMB");
    buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, r->fd, map.offset);
    if (buffer->map == MAP_FAILED) die("mmap");
    memset(buffer->map, 0, buffer->size);
}


static void destroy_buffer(render_shared *r, drm_buffer *buffer)
{
    struct drm_mode_destroy_dumb destroy = {0};

    if (buffer->map && buffer->map != MAP_FAILED)
        munmap(buffer->map, buffer->size);
    if (buffer->framebuffer)
        drmModeRmFB(r->fd, buffer->framebuffer);
    if (buffer->handle) {
        destroy.handle = buffer->handle;
        drmIoctl(r->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}


static void copy_to_scanout(render_shared *r, drm_buffer *buffer)
{
    uint32_t *pixels = (uint32_t *)buffer->map;
    uint32_t white = r->white & 0x00ffffff;
    int y, x;

    memset(buffer->map, 0, buffer->size);
    for (y = 0; y < r->height && y < OUTPUT_HEIGHT; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + (y + 0) * buffer->pitch);
        for (x = 0; x < OUTPUT_WIDTH; x++) {
            const uint8_t *source_row = r->image + y * PITCH(r->width);
            int source_x = (x * (r->width)) / OUTPUT_WIDTH;
            row[x] = source_row[source_x] ? (0xff000000 | white) : 0xff000000;
        }
    }
}


static void page_flip_handler(int fd, unsigned int frame,
                              unsigned int seconds, unsigned int useconds,
                              void *data)
{
    (void)fd;
    (void)frame;
    (void)seconds;
    (void)useconds;
    *(int *)data = 1;
}


static void wait_for_flip(render_shared *r, int *complete)
{
    drmEventContext event = {0};
    struct pollfd pollfd = {r->fd, POLLIN, 0};

    event.version = DRM_EVENT_CONTEXT_VERSION;
    event.page_flip_handler = page_flip_handler;
    while (!*complete) {
        check(poll(&pollfd, 1, -1), "poll DRM page flip");
        check(drmHandleEvent(r->fd, &event), "drmHandleEvent");
    }
}


void *render_thread_func(void *anon_render_shared)
{
    render_shared *r = anon_render_shared;
    int next_buffer = 1;

    while(!r->done) {
        r->draw_func(r->image, next_buffer);
        copy_to_scanout(r, &r->buffer[next_buffer]);
          int complete = 0;

          check(drmModePageFlip(r->fd, r->crtc_id,
                              r->buffer[next_buffer].framebuffer,
                        DRM_MODE_PAGE_FLIP_EVENT, &complete),
              "drmModePageFlip");
        wait_for_flip(r, &complete);
        next_buffer ^= 1;
//        if (r->delay > 0) usleep(r->delay);
    }

    return NULL;
}


static drmModeConnector *find_connector(int fd, uint32_t *connector_id,
                                        drmModeModeInfo *mode)
{
    drmModeRes *resources = drmModeGetResources(fd);
    drmModeConnector *found = NULL;
    int i, n;

    if (!resources) die("drmModeGetResources");
    for (i = 0; i < resources->count_connectors && !found; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes) {
            for (n = 0; n < connector->count_modes; n++) {
                if (connector->modes[n].hdisplay == OUTPUT_WIDTH &&
                    connector->modes[n].vdisplay == OUTPUT_HEIGHT) {
                    *connector_id = connector->connector_id;
                    *mode = connector->modes[n];
                    found = connector;
                    break;
                }
            }
        }
        if (!found) drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);
    return found;
}


void *render_start(int width, int height, int offset, int fixed, InitFunc init_func, DrawFunc draw_func, int delay, int level)
{
    render_shared *r = (render_shared *)calloc(1, sizeof(render_shared));
    drmModeConnector *connector;
    drmModeEncoder *encoder = NULL;
    drmModeRes *resources;
    int i;

    // The staging image already includes OFFSET/FIXED. DRM copies the full
    // image to the scanout buffer, so these legacy partial-update arguments
    // are intentionally unused.
    (void)offset;
    (void)fixed;

    r->draw_func = draw_func;
    r->delay = delay > -1 ? delay : 2000;
    r->width = width;
    r->height = height;
    r->white = ((uint32_t)((level < 0 ? 0 : level > 100 ? 100 : level) * 255 / 100) * 0x010101) | 0xff000000;
    r->image = calloc(1, PITCH(width) * height);
    assert(r->image);
    init_func(r->image);

    r->fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
    if (r->fd < 0) die(DRM_DEVICE);
    check(drmSetClientCap(r->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1),
          "drmSetClientCap");
    connector = find_connector(r->fd, &r->connector_id, &r->mode);
    if (!connector) die("720x576 PAL connector not found");

    resources = drmModeGetResources(r->fd);
    if (!resources) die("drmModeGetResources");
    for (i = 0; i < connector->count_encoders; i++) {
        encoder = drmModeGetEncoder(r->fd, connector->encoders[i]);
        if (encoder) {
            int crtc_index;
            for (crtc_index = 0; crtc_index < resources->count_crtcs; crtc_index++) {
                if (encoder->possible_crtcs & (1 << crtc_index)) {
                    r->crtc_id = resources->crtcs[crtc_index];
                    break;
                }
            }
            if (crtc_index < resources->count_crtcs) break;
        }
        if (encoder) drmModeFreeEncoder(encoder);
        encoder = NULL;
    }
    if (!encoder || !resources->count_crtcs) die("DRM encoder/CRTC not found");
    if (!r->crtc_id) r->crtc_id = encoder->crtc_id;
    r->saved_crtc = drmModeGetCrtc(r->fd, r->crtc_id);
    drmModeFreeEncoder(encoder);
    drmModeFreeResources(resources);
    create_buffer(r, &r->buffer[0]);
    create_buffer(r, &r->buffer[1]);
    copy_to_scanout(r, &r->buffer[0]);
    check(drmModeSetCrtc(r->fd, r->crtc_id, r->buffer[0].framebuffer,
                         0, 0, &r->connector_id, 1, &r->mode),
          "drmModeSetCrtc");
    r->done = 0;
    if (pthread_create(&r->thread, NULL, render_thread_func, r) != 0)
        die("pthread_create");

    return r;
}


void render_stop(void *anon_render_shared) {
    render_shared *r = anon_render_shared;

    r->done = 1;
    pthread_join(r->thread, NULL);
    if (r->saved_crtc && r->saved_crtc->mode_valid)
        drmModeSetCrtc(r->fd, r->crtc_id, r->saved_crtc->buffer_id,
                       r->saved_crtc->x, r->saved_crtc->y,
                       &r->connector_id, 1, &r->saved_crtc->mode);
    destroy_buffer(r, &r->buffer[0]);
    destroy_buffer(r, &r->buffer[1]);
    drmModeFreeCrtc(r->saved_crtc);
    close(r->fd);
    free(r->image);
    free(r);
}
