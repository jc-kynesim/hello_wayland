#define _GNU_SOURCE 1
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "dmabuf_alloc.h"

#define DMABUF_NAME1  "/dev/dma_heap/linux,cma"
#define DMABUF_NAME2  "/dev/dma_heap/reserved"

#define TRACE_ALLOC 0

#define request_log(...)
#define request_debug(...)

struct dmabufs_ctl;
struct dmabuf_h;

struct dmabuf_fns {
    int (*buf_alloc)(struct dmabufs_ctl * dbsc, struct dmabuf_h * dh, size_t size);
    void (*buf_free)(struct dmabuf_h * dh);
    int (*ctl_new)(struct dmabufs_ctl * dbsc);
    void (*ctl_free)(struct dmabufs_ctl * dbsc);
};

struct dmabufs_ctl {
    atomic_int ref_count;
    int fd;
    size_t page_size;
    void * v;
    const struct dmabuf_fns * fns;
};

#define DH_FLAG_FAKE 1

struct dmabuf_h {
    atomic_int ref_count;
    int fd;
    size_t size;
    size_t len;
    void * mapptr;
    void * v;
    const struct dmabuf_fns * fns;
    unsigned int flags;

    void * predel_v;
    dmabuf_predel_fn predel_fn;
};

#if TRACE_ALLOC
static unsigned int total_bufs = 0;
static size_t total_size = 0;
#endif

static void buf_import_free(struct dmabuf_h * dh)
{
    (void)dh;
    // Nothing needed
}

static const struct dmabuf_fns dmabuf_import_fns = {
    // None of the other fns should ever be called
    .buf_free   = buf_import_free,
};

struct dmabuf_h * dmabuf_import_mmap(void * mapptr, size_t size)
{
    struct dmabuf_h *dh;

    if (mapptr == MAP_FAILED)
        return NULL;

    dh = malloc(sizeof(*dh));
    if (!dh)
        return NULL;

    *dh = (struct dmabuf_h) {
        .fd = -1,
        .size = size,
        .mapptr = mapptr,
        .fns = &dmabuf_import_fns,
        .flags = DH_FLAG_FAKE,
    };

    return dh;
}

struct dmabuf_h * dmabuf_import(int fd, size_t size)
{
    struct dmabuf_h *dh;

    fd = dup(fd);
    if (fd < 0  || size == 0)
        return NULL;

    dh = malloc(sizeof(*dh));
    if (!dh) {
        close(fd);
        return NULL;
    }

    *dh = (struct dmabuf_h) {
        .fd = fd,
        .size = size,
        .mapptr = MAP_FAILED,
        .fns = &dmabuf_import_fns,
    };

#if TRACE_ALLOC
    ++total_bufs;
    total_size += dh->size;
    request_log("%s: Import: %zd, total=%zd, bufs=%d\n", __func__, dh->size, total_size, total_bufs);
#endif

    return dh;
}

void dmabuf_free(struct dmabuf_h * dh)
{
    if (!dh)
        return;

#if TRACE_ALLOC
    --total_bufs;
    total_size -= dh->size;
    request_log("%s: Free: %zd, total=%zd, bufs=%d\n", __func__, dh->size, total_size, total_bufs);
#endif

    dh->fns->buf_free(dh);

    if (dh->mapptr != MAP_FAILED && dh->mapptr != NULL)
        munmap(dh->mapptr, dh->size);
    if (dh->fd != -1)
        while (close(dh->fd) == -1 && errno == EINTR)
            /* loop */;
    free(dh);
}

void dmabuf_unref(struct dmabuf_h ** const ppdh)
{
    struct dmabuf_h * const dh = *ppdh;
    int n;

    if (dh == NULL)
        return;
    *ppdh = NULL;

    n = atomic_fetch_sub(&dh->ref_count, 1);
//    fprintf(stderr, "%s[%p]: Ref: %d\n", __func__, dh, n);
    if (n != 0)
        return;

    if (dh->predel_fn) {
        // If we have a predel callback then restore a ref before calling
        dmabuf_ref(dh);
        if (dh->predel_fn(dh, dh->predel_v) != 0)
            return;
    }

    dmabuf_free(dh);
}

struct dmabuf_h * dmabuf_ref(struct dmabuf_h * const dh)
{
    if (dh != NULL)
    {
        int n = atomic_fetch_add(&dh->ref_count, 1);
//        fprintf(stderr, "%s[%p]: Ref: %d\n", __func__, dh, n);
        (void)n;
    }
    return dh;
}

void dmabuf_predel_cb_set(struct dmabuf_h * const dh,
                          const dmabuf_predel_fn predel_fn, void * const predel_v)
{
    dh->predel_fn = predel_fn;
    dh->predel_v  = predel_v;
}

struct dmabuf_h * dmabuf_realloc(struct dmabufs_ctl * dbsc, struct dmabuf_h * old, size_t size)
{
    struct dmabuf_h * dh;
    if (old != NULL) {
        if (old->size >= size) {
            return old;
        }
        dmabuf_free(old);
    }

    if (size == 0 ||
        (dh = malloc(sizeof(*dh))) == NULL)
        return NULL;

    *dh = (struct dmabuf_h){
        .fd = -1,
        .mapptr = MAP_FAILED,
        .fns = dbsc->fns
    };

    if (dh->fns->buf_alloc(dbsc, dh, size) != 0)
        goto fail;

#if TRACE_ALLOC
    ++total_bufs;
    total_size += dh->size;
    request_log("%s: Alloc: %zd, total=%zd, bufs=%d\n", __func__, dh->size, total_size, total_bufs);
#endif

    return dh;

fail:
    free(dh);
    return NULL;
}

int dmabuf_sync(struct dmabuf_h * const dh, unsigned int flags)
{
    struct dma_buf_sync sync = {
        .flags = flags
    };
    if ((dh->flags & DH_FLAG_FAKE) != 0)
        return 0;
    while (ioctl(dh->fd, DMA_BUF_IOCTL_SYNC, &sync) == -1) {
        const int err = errno;
        if (errno == EINTR)
            continue;
        request_log("%s: ioctl failed: flags=%#x\n", __func__, flags);
        return -err;
    }
    return 0;
}

int dmabuf_write_start(struct dmabuf_h * const dh)
{
    return dmabuf_sync(dh, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

int dmabuf_write_end(struct dmabuf_h * const dh)
{
    return dmabuf_sync(dh, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

int dmabuf_read_start(struct dmabuf_h * const dh)
{
    if (!dmabuf_map(dh))
        return -1;
    return dmabuf_sync(dh, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

int dmabuf_read_end(struct dmabuf_h * const dh)
{
    return dmabuf_sync(dh, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}


void * dmabuf_map(struct dmabuf_h * const dh)
{
    if (!dh)
        return NULL;
    if (dh->mapptr != MAP_FAILED)
        return dh->mapptr;
    dh->mapptr = mmap(NULL, dh->size,
              PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_POPULATE,
              dh->fd, 0);
    if (dh->mapptr == MAP_FAILED) {
        request_log("%s: Map failed\n", __func__);
        return NULL;
    }
//    fprintf(stderr, "map to %p\n", dh->mapptr);
    return dh->mapptr;
}

int dmabuf_fd(const struct dmabuf_h * const dh)
{
    if (!dh)
        return -1;
    return dh->fd;
}

size_t dmabuf_size(const struct dmabuf_h * const dh)
{
    if (!dh)
        return 0;
    return dh->size;
}

size_t dmabuf_len(const struct dmabuf_h * const dh)
{
    if (!dh)
        return 0;
    return dh->len;
}

void dmabuf_len_set(struct dmabuf_h * const dh, const size_t len)
{
    dh->len = len;
}

bool dmabuf_is_fake(const struct dmabuf_h * const dh)
{
    return (dh->flags & DH_FLAG_FAKE) != 0;
}

static struct dmabufs_ctl * dmabufs_ctl_new2(const struct dmabuf_fns * const fns)
{
    struct dmabufs_ctl * dbsc = calloc(1, sizeof(*dbsc));

    if (!dbsc)
        return NULL;

    dbsc->fd = -1;
    dbsc->fns = fns;

    dbsc->page_size = (size_t)sysconf(_SC_PAGE_SIZE);
    // Check page size for plausability & power of 2 - set to 4k if not
    if (dbsc->page_size < 0x1000 || dbsc->page_size > 0x1000000 ||
        (dbsc->page_size & (dbsc->page_size - 1)) != 0)
        dbsc->page_size = 0x1000;

    if (fns->ctl_new(dbsc) != 0)
        goto fail;

    return dbsc;

fail:
    free(dbsc);
    return NULL;
}

static void dmabufs_ctl_free(struct dmabufs_ctl * const dbsc)
{
    request_debug(NULL, "Free dmabuf ctl\n");

    dbsc->fns->ctl_free(dbsc);

    free(dbsc);
}

void dmabufs_ctl_unref(struct dmabufs_ctl ** const pDbsc)
{
    struct dmabufs_ctl * const dbsc = *pDbsc;

    if (!dbsc)
        return;
    *pDbsc = NULL;

    if (atomic_fetch_sub(&dbsc->ref_count, 1) != 0)
        return;

    dmabufs_ctl_free(dbsc);
}

struct dmabufs_ctl * dmabufs_ctl_ref(struct dmabufs_ctl * const dbsc)
{
    atomic_fetch_add(&dbsc->ref_count, 1);
    return dbsc;
}

//-----------------------------------------------------------------------------
//
// Alloc dmabuf via CMA

static int ctl_cma_new2(struct dmabufs_ctl * dbsc, const char * const * names)
{
    for (; *names != NULL; ++names)
    {
        while ((dbsc->fd = open(*names, O_RDWR | __O_CLOEXEC)) == -1 &&
               errno == EINTR)
            /* Loop */;
        if (dbsc->fd != -1)
            return 0;
    }
    request_log("Unable to open any dma_heap device\n");
    return -1;
}

static int ctl_cma_new(struct dmabufs_ctl * dbsc)
{
    static const char * const names[] = {
        "/dev/dma_heap/vidbuf_cached",
        "/dev/dma_heap/linux,cma",
        "/dev/dma_heap/reserved",
        NULL
    };

    return ctl_cma_new2(dbsc, names);
}

static void ctl_cma_free(struct dmabufs_ctl * dbsc)
{
    if (dbsc->fd != -1)
        while (close(dbsc->fd) == -1 && errno == EINTR)
            /* loop */;

}

static int buf_cma_alloc(struct dmabufs_ctl * const dbsc, struct dmabuf_h * dh, size_t size)
{
    struct dma_heap_allocation_data data = {
        .len = (size + dbsc->page_size - 1) & ~(dbsc->page_size - 1),
        .fd = 0,
        .fd_flags = O_RDWR,
        .heap_flags = 0
    };

    while (ioctl(dbsc->fd, DMA_HEAP_IOCTL_ALLOC, &data)) {
        int err = errno;
        request_log("Failed to alloc %" PRIu64 " from dma-heap(fd=%d): %d (%s)\n",
                (uint64_t)data.len,
                dbsc->fd,
                err,
                strerror(err));
        if (err == EINTR)
            continue;
        return -err;
    }

    dh->fd = data.fd;
    dh->size = (size_t)data.len;

//    fprintf(stderr, "%s: size=%#zx, ftell=%#zx\n", __func__,
//            dh->size, (size_t)lseek(dh->fd, 0, SEEK_END));

    return 0;
}

static void buf_cma_free(struct dmabuf_h * dh)
{
    (void)dh;
    // Nothing needed
}

static const struct dmabuf_fns dmabuf_cma_fns = {
    .buf_alloc  = buf_cma_alloc,
    .buf_free   = buf_cma_free,
    .ctl_new    = ctl_cma_new,
    .ctl_free   = ctl_cma_free,
};

struct dmabufs_ctl * dmabufs_ctl_new(void)
{
    request_debug(NULL, "Dmabufs using CMA\n");;
    return dmabufs_ctl_new2(&dmabuf_cma_fns);
}

//-----------------------------------------------------------------------------
//
// Alloc "dmabuf" via shm (one file per alloc)

static int ctl_shm_new(struct dmabufs_ctl * dbsc)
{
    (void)dbsc;
    return 0;
}

static void ctl_shm_free(struct dmabufs_ctl * dbsc)
{
    (void)dbsc;
}

static int buf_shm_alloc(struct dmabufs_ctl * const dbsc, struct dmabuf_h * dh, size_t size)
{
    int fd;

#if 0
    const char * const tmpdir = "/tmp";
    fd = open(tmpdir, __O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        const int err = errno;
        request_log("Failed to open tmp file in %s: %s\n", tmpdir, strerror(err));
        return -err;
    }
#else
    fd = memfd_create("vlc/shm_buf", 0);
    if (fd == -1) {
        const int err = errno;
        request_log("Failed to create memfd: %s\n", strerror(err));
        return -err;
    }
#endif

    // Round up to page size
    size = (size + dbsc->page_size - 1) & ~(dbsc->page_size - 1);

    if (ftruncate(fd, (off_t)size) != 0)
    {
        const int err = errno;
        request_log("Failed to extend tmp file to %zd: %s\n", size, strerror(err));
        return -err;
    }

    dh->fd = fd;
    dh->size = size;
    dh->flags = DH_FLAG_FAKE;

//    fprintf(stderr, "%s: size=%#zx, ftell=%#zx\n", __func__,
//            dh->size, (size_t)lseek(dh->fd, 0, SEEK_END));

    return 0;
}

static void buf_shm_free(struct dmabuf_h * dh)
{
    (void)dh;
    // Nothing needed
}

static const struct dmabuf_fns dmabuf_shm_fns = {
    .buf_alloc  = buf_shm_alloc,
    .buf_free   = buf_shm_free,
    .ctl_new    = ctl_shm_new,
    .ctl_free   = ctl_shm_free,
};

struct dmabufs_ctl * dmabufs_shm_new()
{
    request_debug(NULL, "Dmabufs using SHM\n");;
    return dmabufs_ctl_new2(&dmabuf_shm_fns);
}


