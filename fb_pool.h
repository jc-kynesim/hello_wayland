#ifndef _FB_POOL_H
#define _FB_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fb_pool_s;
typedef struct fb_pool_s fb_pool_t;

struct wo_fb_s;
struct wo_env_s;

// fb pool

void fb_pool_unref(fb_pool_t ** const pppool);
fb_pool_t * fb_pool_ref(fb_pool_t * const pool);

// Allocate a fb from the pool
// Allocations need not be all of the same size but no guarantees are made about
// efficient memory use if this is the case
struct wo_fb_s * fb_pool_fb_new(fb_pool_t * const pool, uint32_t width, uint32_t height, uint32_t fmt, uint64_t mod);
// Marks the pool as dead & unrefs this reference
//   No allocs will succeed after this
//   All free fbs are unrefed
void fb_pool_kill(fb_pool_t ** const pppool);

fb_pool_t * fb_pool_new_fbs(struct wo_env_s * const woe, unsigned int total_fbs_max);

#ifdef __cplusplus
}
#endif

#endif
