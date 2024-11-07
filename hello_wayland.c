/*
 * Copyright (c) 2017 Jun Zhao
 * Copyright (c) 2017 Kaixuan Liu
 *
 * HW Acceleration API (video decoding) decode sample
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * HW-Accelerated decoding example.
 *
 * @example hw_decode.c
 * This example shows how to do HW-accelerated decoding with output
 * frames from the HW video surfaces.
 */
#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "init_window.h"

static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;
static long frames = 0;
static bool no_wait = false;

static AVFilterContext *buffersink_ctx = NULL;
static AVFilterContext *buffersrc_ctx = NULL;
static AVFilterGraph *filter_graph = NULL;

static AVDictionary *codec_opts = NULL;
static long ffdebug_level = -1L;

static int64_t
time_us()
{
    struct timespec ts = {0,0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)(ts.tv_nsec / 1000) + (int64_t)(ts.tv_sec * 1000000);

}

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    ctx->hw_frames_ctx = NULL;
    // ctx->hw_device_ctx gets freed when we call avcodec_free_context
    if ((err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    (void)ctx;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static int64_t
frame_pts(const AVFrame * const frame)
{
    return frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
}

static void
display_wait(const AVFrame * const frame, const AVRational time_base)
{
    static int64_t base_pts = 0;
    static int64_t base_now = 0;
    static int64_t last_conv = 0;

    int64_t now = time_us();
    int64_t now_delta = now - base_now;
    int64_t pts = frame_pts(frame);
    int64_t pts_delta = pts - base_pts;
    // If we haven't been given any clues then guess 60fps
    int64_t pts_conv = (pts == AV_NOPTS_VALUE || time_base.den == 0 || time_base.num == 0) ?
        last_conv + 1000000 / 60 :
        av_rescale_q(pts_delta, time_base, (AVRational) {1, 1000000});  // frame->timebase seems invalid currently
    int64_t delta = pts_conv - now_delta;

    last_conv = pts_conv;

//    printf("PTS_delta=%" PRId64 ", Now_delta=%" PRId64 ", TB=%d/%d, Delta=%" PRId64 "\n", pts_delta, now_delta, time_base.num, time_base.den, delta);

    if (delta < 0 || delta > 6000000) {
        base_pts = pts;
        base_now = now;
        return;
    }

    if (delta > 0)
        usleep(delta);
}

static int decode_write(const AVStream * const stream,
                        AVCodecContext * const avctx,
                        vid_out_env_t * const dpo,
                        AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    for (;;) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        // push the decoded frame into the filtergraph if it exists
        if (filter_graph != NULL &&
            (ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
            fprintf(stderr, "Error while feeding the filtergraph\n");
            goto fail;
        }

        do {
            AVRational time_base = stream->time_base;
            if (filter_graph != NULL) {
                av_frame_unref(frame);
                ret = av_buffersink_get_frame(buffersink_ctx, frame);
                if (ret == AVERROR(EAGAIN)) {
                    ret = 0;
                    break;
                }
                if (ret < 0) {
                    if (ret != AVERROR_EOF)
                        fprintf(stderr, "Failed to get frame: %s", av_err2str(ret));
                    goto fail;
                }
                vidout_wayland_modeset(dpo, av_buffersink_get_w(buffersink_ctx), av_buffersink_get_h(buffersink_ctx), av_buffersink_get_time_base(buffersink_ctx));
                time_base = av_buffersink_get_time_base(buffersink_ctx);
            }
            else {
                vidout_wayland_modeset(dpo, avctx->coded_width, avctx->coded_height, avctx->framerate);
            }

            if (!no_wait)
                display_wait(frame, time_base);
            vidout_wayland_display(dpo, frame);

            if (output_file != NULL) {
                AVFrame *tmp_frame;

                if (frame->format == hw_pix_fmt) {
                    /* retrieve data from GPU to CPU */
                    if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                        fprintf(stderr, "Error transferring the data to system memory\n");
                        goto fail;
                    }
                    tmp_frame = sw_frame;
                } else
                    tmp_frame = frame;

                size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
                                                tmp_frame->height, 1);
                buffer = av_malloc(size);
                if (!buffer) {
                    fprintf(stderr, "Can not alloc buffer\n");
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                ret = av_image_copy_to_buffer(buffer, size,
                                              (const uint8_t * const *)tmp_frame->data,
                                              (const int *)tmp_frame->linesize, tmp_frame->format,
                                              tmp_frame->width, tmp_frame->height, 1);
                if (ret < 0) {
                    fprintf(stderr, "Can not copy image to buffer\n");
                    goto fail;
                }

                if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
                    fprintf(stderr, "Failed to dump raw data.\n");
                    goto fail;
                }
            }
        } while (buffersink_ctx != NULL);  // Loop if we have a filter to drain

        if (frames == 0 || --frames == 0)
            ret = -1;

    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
    return 0;
}

// Copied almost directly from ffmpeg filtering_video.c example
static int init_filters(const AVStream * const stream,
                        const AVCodecContext * const dec_ctx,
                        const char * const filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = stream->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE };

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static void log_callback_help(void *ptr, int level, const char *fmt, va_list vl)
{
    (void)ptr;
    if (level <= ffdebug_level * 8) {
        static uint64_t t0 = 0;
        uint64_t now = time_us();
        if (t0 == 0)
            t0 = now;
        printf("%4"PRId64".%04d: [%d] ", (now - t0)/1000000, (unsigned int)((now - t0) % 1000000) / 1000, level / 8);
        vfprintf(stdout, fmt, vl);
    }
}

void usage()
{
    fprintf(stderr,
            "Usage: hello_wayland [-e]\n"
            "                     [-l <loop_count>] [-f <frames>] [-o <yuv_output_file>]\n"
            "                     [--deinterlace] [--pace-input <hz>] [--fullscreen]\n"
            "                     "
#if HAS_RUNTICKER
            "[--ticker <text>] "
#endif
#if HAS_RUNCUBE
            "[--cube] "
#endif
            "[--no-wait]\n"
            "                     <input file> [<input_file> ...]\n\n"
            " -e        Use EGL to render video (otherwise direct dmabuf)\n"
            " -l        Loop video playback <loop_count> times. -1 means forever\n"
            " --cube    Show rotating cube\n"
            " --ticker  Show scrolling ticker with <text> repeated indefinitely\n"
            " --no-wait Decode at max speed, do not wait for display\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVCodecContext *decoder_ctx = NULL;
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec *decoder = NULL;
#else
    AVCodec *decoder = NULL;
#endif
    AVPacket packet;
    enum AVHWDeviceType type;
    const char * in_file;
    char * const * in_filelist;
    unsigned int in_count;
    unsigned int in_n = 0;
    const char * hwdev = "drm";
    int i;
    vid_out_env_t * dpo;
    long loop_count = 1;
    long frame_count = -1;
    const char * out_name = NULL;
    bool wants_deinterlace = false;
    long pace_input_hz = 0;
    bool try_hw = true;
    bool use_dmabuf = true;
    bool fullscreen = false;
#if HAS_RUNCUBE
    bool wants_cube = false;
#endif
#if HAS_RUNTICKER
    const char * ticker_text = NULL;
#endif

    {
        char * const * a = argv + 1;
        int n = argc - 1;

        while (n-- > 0 && a[0][0] == '-') {
            const char *arg = *a++;
            char *e;

            if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loop") == 0) {
                if (n == 0)
                    usage();
                loop_count = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
            else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--frames") == 0) {
                if (n == 0)
                    usage();
                frame_count = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
            else if (strcmp(arg, "-F") == 0 || strcmp(arg, "--fullscreen") == 0) {
                fullscreen = true;
            }
            else if (strcmp(arg, "-o") == 0) {
                if (n == 0)
                    usage();
                out_name = *a;
                --n;
                ++a;
            }
            else if (strcmp(arg, "-O") == 0) {
                if (av_dict_parse_string(&codec_opts, *a, "=", ":", 0) != 0) {
                    fprintf(stderr, "Bad codec opts '%s': usage: <opt>=<value[:<opt>=<value>]^\n", *a);
                    usage();
                }
                --n;
                ++a;
            }
            else if (strcmp(arg, "-e") == 0) {
                use_dmabuf = false;
            } else if (strcmp(arg, "--pace-input") == 0) {
                if (n == 0)
                    usage();
                pace_input_hz = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
            else if (strcmp(arg, "--deinterlace") == 0) {
                wants_deinterlace = true;
            }
            else if (strcmp(arg, "--ffdebug") == 0) {
                if (n == 0)
                    usage();
                ffdebug_level = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
#if HAS_RUNCUBE
            else if (strcmp(arg, "--cube") == 0) {
                wants_cube = true;
            }
#endif
#if HAS_RUNTICKER
            else if (strcmp(arg, "--ticker") == 0) {
                if (n == 0)
                    usage();
                ticker_text = *a;
                --n;
                ++a;
            }
#endif
            else if (strcmp(arg, "--no-wait") == 0) {
                no_wait = true;
            }
            else if (strcmp(arg, "--") == 0) {
                --n;  // If we are going to break out then need to dec count like in the while
                break;
            }
            else
                usage();
        }

        // Last args are input files
        if (n < 0)
            usage();

        in_filelist = a;
        in_count = n + 1;
        if (loop_count > 0)
            loop_count *= in_count;
    }

    if (ffdebug_level >= 0)
        av_log_set_callback(log_callback_help);

    type = av_hwdevice_find_type_by_name(hwdev);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "Device type %s is not supported.\n", hwdev);
        fprintf(stderr, "Available device types:");
        while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    {
        unsigned int flags =
            (fullscreen ? WOUT_FLAG_FULLSCREEN : 0) |
            (no_wait ? WOUT_FLAG_NO_WAIT : 0);
        dpo = use_dmabuf ? dmabuf_wayland_out_new(flags) : vidout_wayland_new(flags);
        if (dpo == NULL) {
            fprintf(stderr, "Failed to open egl_wayland output\n");
            return 1;
        }
    }

    /* open the file to dump raw data */
    if (out_name != NULL) {
        if ((output_file = fopen(out_name, "w+")) == NULL) {
            fprintf(stderr, "Failed to open output file %s: %s\n", out_name, strerror(errno));
            return -1;
        }
    }

#if HAS_RUNTICKER
    if (ticker_text != NULL && *ticker_text != '\0')
        vidout_wayland_runticker(dpo, ticker_text);
#endif
#if HAS_RUNCUBE
    if (wants_cube)
        vidout_wayland_runcube(dpo);
#endif

loopy:
    in_file = in_filelist[in_n];
    if (++in_n >= in_count)
        in_n = 0;

    /* open the input file */
    if (avformat_open_input(&input_ctx, in_file, NULL, NULL) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", in_file);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

retry_hw:
    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream = ret;

    hw_pix_fmt = AV_PIX_FMT_NONE;
    if (!try_hw) {
        /* Nothing */
    }
    else if (decoder->id == AV_CODEC_ID_H264) {
        if ((decoder = avcodec_find_decoder_by_name("h264_v4l2m2m")) == NULL)
            fprintf(stderr, "Cannot find the h264 v4l2m2m decoder\n");
        else
            hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;
    }
    else {
        for (i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                fprintf(stderr, "Decoder %s does not support device type %s.\n",
                        decoder->name, av_hwdevice_get_type_name(type));
                break;
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == type) {
                hw_pix_fmt = config->pix_fmt;
                break;
            }
        }
    }

    if (hw_pix_fmt == AV_PIX_FMT_NONE && try_hw) {
        fprintf(stderr, "No h/w format found - trying s/w\n");
        try_hw = false;
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;

    if (try_hw) {
        decoder_ctx->get_format  = get_hw_format;

        if (hw_decoder_init(decoder_ctx, type) < 0)
            return -1;

        decoder_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
        decoder_ctx->sw_pix_fmt = AV_PIX_FMT_NONE;

        decoder_ctx->thread_count = 3;
    }
    else {
        decoder_ctx->get_buffer2 = vidout_wayland_get_buffer2;
        decoder_ctx->opaque = dpo;
        decoder_ctx->thread_count = 0; // FFmpeg will pick a default
    }
    decoder_ctx->flags = 0;
    // Pick any threading method
    decoder_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

#if LIBAVCODEC_VERSION_MAJOR < 60
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    decoder_ctx->thread_safe_callbacks = 1;
#pragma GCC diagnostic pop
#endif

    if ((ret = avcodec_open2(decoder_ctx, decoder, &codec_opts)) < 0) {
        if (try_hw) {
            try_hw = false;
            avcodec_free_context(&decoder_ctx);

            printf("H/w init failed - trying s/w\n");
            goto retry_hw;
        }
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    printf("Pixfmt after init: %s / %s\n", av_get_pix_fmt_name(decoder_ctx->pix_fmt), av_get_pix_fmt_name(decoder_ctx->sw_pix_fmt));

    if (wants_deinterlace) {
        if (init_filters(video, decoder_ctx, "deinterlace_v4l2m2m") < 0) {
            fprintf(stderr, "Failed to init deinterlace\n");
            return -1;
        }
    }

    /* actual decoding and dump the raw data */
    {
        int64_t t0 = time_us() + 3000; // Allow a few ms so we aren't behind at startup
        int pts_seen = 0;
        int64_t fake_ts = 0;

        frames = frame_count;
        while (ret >= 0) {
            if ((ret = av_read_frame(input_ctx, &packet)) < 0)
                break;

            if (video_stream == packet.stream_index) {
                if (pace_input_hz > 0) {
                    const int64_t now = time_us();
                    if (now < t0)
                        usleep(t0 - now);
                    else
                        fprintf(stderr, "input pace failure by %"PRId64"us\n", now - t0);

                    t0 += 1000000 / pace_input_hz;

                    if (packet.pts != AV_NOPTS_VALUE) {
                        pts_seen = 1;
                    }
                    else if (!pts_seen) {
                        packet.dts = fake_ts;
                        packet.pts = fake_ts;
                        fake_ts += 90000 / pace_input_hz;
                    }
                }

                ret = decode_write(video, decoder_ctx, dpo, &packet);
            }

            av_packet_unref(&packet);
        }
    }

    /* flush the decoder */
    packet.data = NULL;
    packet.size = 0;
    ret = decode_write(video, decoder_ctx, dpo, &packet);
    av_packet_unref(&packet);

    if (output_file)
        fclose(output_file);
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);

    if (loop_count == -1 || --loop_count > 0)
        goto loopy;

    vidout_wayland_delete(dpo);
    return 0;
}
