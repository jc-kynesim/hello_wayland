#ifndef _FREETYPE_RUNTICKER_H
#define _FREETYPE_RUNTICKER_H

#ifdef __cplusplus
extern "C" {
#endif

struct runticker_env_s;
typedef struct runticker_env_s runticker_env_t;

struct wo_window_s;

runticker_env_t * runticker_start(struct wo_window_s * const woe,
                                  unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                                  const char * const text,
                                  const char * const fontfile);

void runticker_stop(runticker_env_t ** const ppDfte);

#ifdef __cplusplus
}
#endif

#endif

