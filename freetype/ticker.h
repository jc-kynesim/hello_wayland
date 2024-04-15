#ifndef _FREETYPE_TICKER_H
#define _FREETYPE_TICKER_H

#include <wayout.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ticker_env_s;
typedef struct ticker_env_s ticker_env_t;

typedef int (* ticker_next_char_fn)(void * v);

ticker_env_t * ticker_new(struct wo_window_s * wowin, const wo_rect_t pos, const wo_rect_t win_pos);
int ticker_set_face(ticker_env_t * const te, const char * const filename);
void ticker_next_char_cb_set(ticker_env_t * const ticker, const ticker_next_char_fn fn, void * const v);
void ticker_commit_cb_set(ticker_env_t *const te, void (* commit_cb)(void * v), void * commit_v);
int ticker_init(ticker_env_t *const te);

int ticker_run(ticker_env_t * const ticker);
void ticker_delete(ticker_env_t ** ppTicker);

#ifdef __cplusplus
}
#endif

#endif
