#include <stdint.h>

#include "libavutil/rational.h"

struct vid_out_env_s;
typedef struct vid_out_env_s vid_out_env_t;

#define WOUT_FLAG_FULLSCREEN 1
#define WOUT_FLAG_NO_WAIT    2

struct AVFrame;
struct AVCodecContext;

int vidout_wayland_get_buffer2(struct AVCodecContext *s, struct AVFrame *frame, int flags);
void vidout_wayland_modeset(struct vid_out_env_s * dpo, int w, int h, AVRational frame_rate);
int vidout_wayland_display(struct vid_out_env_s * dpo, struct AVFrame * frame);
// Returns the number of frames that have been queued by _display but
// not yet released
int vidout_wayland_in_flight(const vid_out_env_t * dpo);
struct vid_out_env_s * vidout_wayland_new(unsigned int flags);

struct vid_out_env_s * dmabuf_wayland_out_new(unsigned int flags);
void vidout_wayland_delete(struct vid_out_env_s * dpo);

void vidout_wayland_runticker(struct vid_out_env_s * dpo, const char * text);
void vidout_wayland_runcube(struct vid_out_env_s * dpo);

