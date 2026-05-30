#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <linux/videodev2.h>
#include <unistd.h>

#define NBUFS   4
#define NFRAMES 100
#define DEV     "/dev/video0"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ── MMAP capture ─────────────────────────────────────────────────────────── */

static double bench_mmap(int fd, uint32_t w, uint32_t h)
{
    struct v4l2_requestbuffers req = {
        .count  = NBUFS,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("REQBUFS mmap"); return -1;
    }

    void *bufs[NBUFS];
    for (int i = 0; i < NBUFS; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, buf.m.offset);
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    double t0 = now_ms();
    for (int f = 0; f < NFRAMES; f++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        ioctl(fd, VIDIOC_DQBUF, &buf);
        /* touch first byte to ensure page is accessed */
        volatile uint8_t x = ((uint8_t *)bufs[buf.index])[0]; (void)x;
        ioctl(fd, VIDIOC_QBUF, &buf);
    }
    double elapsed = now_ms() - t0;

    ioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < NBUFS; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        munmap(bufs[i], buf.length);
    }
    req.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    return elapsed;
}

/* ── USERPTR capture ──────────────────────────────────────────────────────── */

static double bench_userptr(int fd, uint32_t w, uint32_t h)
{
    uint32_t size = w * h * 3;   /* RGB24 */

    struct v4l2_requestbuffers req = {
        .count  = NBUFS,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("REQBUFS userptr"); return -1;
    }

    /* Pre-allocate page-aligned userspace buffers */
    void *bufs[NBUFS];
    for (int i = 0; i < NBUFS; i++) {
        bufs[i] = aligned_alloc(4096, size);
        if (!bufs[i]) { perror("aligned_alloc"); return -1; }
        /* pre-fault all pages so they are resident */
        memset(bufs[i], 0, size);

        struct v4l2_buffer buf = {
            .type      = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory    = V4L2_MEMORY_USERPTR,
            .index     = i,
            .length    = size,
            .m.userptr = (unsigned long)bufs[i],
        };
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    double t0 = now_ms();
    for (int f = 0; f < NFRAMES; f++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
        };
        ioctl(fd, VIDIOC_DQBUF, &buf);
        volatile uint8_t x = ((uint8_t *)buf.m.userptr)[0]; (void)x;
        struct v4l2_buffer qbuf = {
            .type      = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory    = V4L2_MEMORY_USERPTR,
            .index     = buf.index,
            .length    = size,
            .m.userptr = buf.m.userptr,
        };
        ioctl(fd, VIDIOC_QBUF, &qbuf);
    }
    double elapsed = now_ms() - t0;

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    req.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &req);
    for (int i = 0; i < NBUFS; i++)
        free(bufs[i]);

    return elapsed;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--mmap|--userptr|--both] [device]\n", prog);
    fprintf(stderr, "  --mmap     benchmark MMAP only\n");
    fprintf(stderr, "  --userptr  benchmark USERPTR only\n");
    fprintf(stderr, "  --both     benchmark both and compare (default)\n");
}

int main(int argc, char *argv[])
{
    const char *dev = DEV;
    int do_mmap = 1, do_userptr = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mmap"))    { do_mmap = 1; do_userptr = 0; }
        else if (!strcmp(argv[i], "--userptr")) { do_mmap = 0; do_userptr = 1; }
        else if (!strcmp(argv[i], "--both"))    { do_mmap = 1; do_userptr = 1; }
        else if (!strcmp(argv[i], "--help"))    { usage(argv[0]); return 0; }
        else dev = argv[i];
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    uint32_t w = fmt.fmt.pix.width;
    uint32_t h = fmt.fmt.pix.height;
    printf("Device: %s  Format: %ux%u  frames: %d  buffers: %d\n\n",
           dev, w, h, NFRAMES, NBUFS);

    double t_mmap = 0, t_userptr = 0;

    if (do_mmap) {
        printf("Benchmarking MMAP ...\n");
        t_mmap = bench_mmap(fd, w, h);
        if (t_mmap < 0) { close(fd); return 1; }
        printf("  total: %.1f ms  per-frame: %.2f ms  (%.1f fps)\n\n",
               t_mmap, t_mmap / NFRAMES, NFRAMES / (t_mmap / 1e3));
    }

    if (do_userptr) {
        printf("Benchmarking USERPTR ...\n");
        t_userptr = bench_userptr(fd, w, h);
        if (t_userptr < 0) { close(fd); return 1; }
        printf("  total: %.1f ms  per-frame: %.2f ms  (%.1f fps)\n\n",
               t_userptr, t_userptr / NFRAMES, NFRAMES / (t_userptr / 1e3));
    }

    if (do_mmap && do_userptr)
        printf("USERPTR vs MMAP: %.1f%%  (%+.1f ms total)\n",
               (t_userptr / t_mmap - 1.0) * 100.0,
               t_userptr - t_mmap);

    close(fd);
    return 0;
}