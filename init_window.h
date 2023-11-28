#include "libavutil/frame.h"

struct wayland_out_env_s;
typedef struct wayland_out_env_s egl_wayland_out_env_t;

#define WOUT_FLAG_FULLSCREEN 1
#define WOUT_FLAG_NO_WAIT    2

void egl_wayland_out_modeset(struct wayland_out_env_s * dpo, int w, int h, AVRational frame_rate);
int egl_wayland_out_display(struct wayland_out_env_s * dpo, AVFrame * frame);
struct wayland_out_env_s * egl_wayland_out_new(unsigned int flags);
struct wayland_out_env_s * dmabuf_wayland_out_new(unsigned int flags);
void egl_wayland_out_delete(struct wayland_out_env_s * dpo);


