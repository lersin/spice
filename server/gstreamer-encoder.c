/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Jeremy White
   Copyright (C) 2015 Francois Gouget

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "red-common.h"
#include "video-encoder.h"
#include "utils.h"


#define SPICE_GST_DEFAULT_FPS 30

#define DO_ZERO_COPY


typedef struct {
    SpiceBitmapFmt spice_format;
    const char *format;
    uint32_t bpp;
} SpiceFormatForGStreamer;

typedef struct SpiceGstVideoBuffer {
    VideoBuffer base;
    GstBuffer *gst_buffer;
    GstMapInfo map;
} SpiceGstVideoBuffer;

typedef struct {
    uint32_t mm_time;
    uint64_t duration;
    uint32_t size;
} SpiceGstFrameInformation;

typedef enum SpiceGstBitRateStatus {
    SPICE_GST_BITRATE_DECREASING,
    SPICE_GST_BITRATE_INCREASING,
    SPICE_GST_BITRATE_STABLE,
} SpiceGstBitRateStatus;

typedef struct SpiceGstEncoder {
    VideoEncoder base;

    /* Callbacks to adjust the refcount of the bitmap being encoded. */
    bitmap_ref_t bitmap_ref;
    bitmap_unref_t bitmap_unref;

#ifdef DO_ZERO_COPY
    GAsyncQueue *unused_bitmap_opaques;
#endif

    /* Rate control callbacks */
    VideoEncoderRateControlCbs cbs;

    /* Spice's initial bit rate estimation in bits per second. */
    uint64_t starting_bit_rate;

    /* ---------- Video characteristics ---------- */

    uint32_t width;
    uint32_t height;
    const SpiceFormatForGStreamer *format;
    SpiceBitmapFmt spice_format;

    /* Number of consecutive frame encoding errors. */
    uint32_t errors;

    /* ---------- GStreamer pipeline ---------- */

    /* Pointers to the GStreamer pipeline elements. If pipeline is NULL the
     * other pointers are invalid.
     */
    GstElement *pipeline;
    GstAppSrc *appsrc;
    GstElement *gstenc;
    GstAppSink *appsink;

    /* If src_caps is NULL the pipeline has not been configured yet. */
    GstCaps *src_caps;

    /* The frame counter for GStreamer buffers */
    uint32_t frame;

    /* The GStreamer bit rate. */
    uint64_t video_bit_rate;

    /* Don't bother changing the GStreamer bit rate if close enough. */
#   define SPICE_GST_VIDEO_BITRATE_MARGIN 0.05


    /* ---------- Encoded frame statistics ---------- */

    /* Should be >= than FRAME_STATISTICS_COUNT. This is also used to annotate
     * the client report debug traces with bit rate information.
     */
#   define SPICE_GST_HISTORY_SIZE 60

    /* A circular buffer containing the past encoded frames information. */
    SpiceGstFrameInformation history[SPICE_GST_HISTORY_SIZE];

    /* The indices of the oldest and newest frames in the history buffer. */
    uint32_t history_first;
    uint32_t history_last;

    /* How many frames to take into account when computing the effective
     * bit rate, average frame size, etc. This should be large enough so the
     * I and P frames average out, and short enough for it to reflect the
     * current situation.
     */
#   define SPICE_GST_FRAME_STATISTICS_COUNT 21

    /* The index of the oldest frame taken into account for the statistics. */
    uint32_t stat_first;

    /* Used to compute the average frame encoding time. */
    uint64_t stat_duration_sum;

    /* Used to compute the average frame size. */
    uint64_t stat_size_sum;

    /* Tracks the maximum frame size. */
    uint32_t stat_size_max;


    /* ---------- Encoder bit rate control ----------
     *
     * GStreamer encoders don't follow the specified video_bit_rate very
     * closely. These fields are used to ensure we don't exceed the desired
     * stream bit rate, regardless of the GStreamer encoder's output.
     */

    /* The bit rate target for the outgoing network stream. (bits per second) */
    uint64_t bit_rate;

    /* The minimum bit rate / bit rate increment. */
#   define SPICE_GST_MIN_BITRATE (128 * 1024)

    /* The default bit rate */
#   define SPICE_GST_DEFAULT_BITRATE (8 * 1024 * 1024)

    /* The bit rate control is performed using a virtual buffer to allow short
     * term variations: bursts are allowed until the virtual buffer is full.
     * Then frames are dropped to limit the bit rate. VBUFFER_SIZE defines the
     * size of the virtual buffer in milliseconds worth of data.
     */
#   define SPICE_GST_VBUFFER_SIZE 300

    int32_t vbuffer_size;
    int32_t vbuffer_free;

    /* When dropping frames, this is set to the minimum mm_time of the next
     * frame to encode. Otherwise set to zero.
     */
    uint32_t next_frame_mm_time;

    /* Defines the minimum allowed fps. */
#   define SPICE_GST_MAX_PERIOD (NSEC_PER_SEC / 3)

    /* How big of a margin to take to cover for latency jitter. */
#   define SPICE_GST_LATENCY_MARGIN 0.1


    /* ---------- Network bit rate control ----------
     *
     * State information for figuring out the optimal bit rate for the current
     * network conditions.
     */

    /* The mm_time of the last bit rate change. */
    uint32_t last_change;

    /* How much to reduce the bit rate in case of network congestion. */
#   define SPICE_GST_BITRATE_CUT 2
#   define SPICE_GST_BITRATE_REDUCE (4.0 / 3.0)

    /* Never increase the bit rate by more than this amount (bits per second). */
#   define SPICE_GST_BITRATE_MAX_STEP (1024 * 1024)

    /* The maximum bit rate that one can maybe use without causing network
     * congestion.
     */
    uint64_t max_bit_rate;

    /* The last bit rate that let us recover from network congestion. */
    uint64_t min_bit_rate;

    /* Defines when the spread between max_bit_rate and min_bit_rate has been
     * narrowed down enough. Note that this value should be large enough for
     * min_bit_rate to allow recovery from network congestion in a reasonable
     * time frame, and to absorb transient traffic spikes (potentially from
     * other sources).
     * This is also used as a multiplier for the video_bit_rate so it does not
     * have to be changed too often.
     */
#   define SPICE_GST_BITRATE_MARGIN SPICE_GST_BITRATE_REDUCE

    /* Whether the bit rate was last decreased, increased or kept stable. */
    SpiceGstBitRateStatus status;

    /* The network bit rate control uses an AIMD scheme (Additive Increase,
     * Multiplicative Decrease). The increment step depends on the spread
     * between the minimum and maximum bit rates.
     */
    uint64_t bit_rate_step;

    /* How often to increase the bit rate. */
    uint32_t increase_interval;

#   define SPICE_GST_BITRATE_UP_INTERVAL (MSEC_PER_SEC * 2)
#   define SPICE_GST_BITRATE_UP_CLIENT_STABLE (MSEC_PER_SEC * 60 * 2)
#   define SPICE_GST_BITRATE_UP_SERVER_STABLE (MSEC_PER_SEC * 3600 * 4)
#   define SPICE_GST_BITRATE_UP_RESET_MAX (MSEC_PER_SEC * 30)


    /* ---------- Client feedback ---------- */

    /* TRUE if gst_encoder_client_stream_report() is being called. */
    gboolean has_client_reports;

    /* The margin is the amount of time between the reception of a piece of
     * media data by the client and the time when it should be played/displayed.
     * Increasing the bit rate increases the transmission time and thus reduces
     * the margin.
     */
    int32_t last_video_margin;
    int32_t max_video_margin;
    uint32_t max_audio_margin;

#   define SPICE_GST_VIDEO_MARGIN_GOOD 0.75
#   define SPICE_GST_VIDEO_MARGIN_AVERAGE 0.5
#   define SPICE_GST_VIDEO_MARGIN_BAD 0.3

#   define SPICE_GST_VIDEO_DELTA_BAD 0.2
#   define SPICE_GST_VIDEO_DELTA_AVERAGE 0.15

#   define SPICE_GST_AUDIO_MARGIN_BAD 0.5
#   define SPICE_GST_AUDIO_VIDEO_RATIO 1.25


    /* ---------- Server feedback ---------- */

    /* How many frames were dropped by the server since the last encoded frame. */
    uint32_t server_drops;
} SpiceGstEncoder;


/* ---------- The SpiceGstVideoBuffer implementation ---------- */

static void spice_gst_video_buffer_free(VideoBuffer *video_buffer)
{
    SpiceGstVideoBuffer *buffer = (SpiceGstVideoBuffer*)video_buffer;
    if (buffer->gst_buffer) {
        gst_buffer_unref(buffer->gst_buffer);
    }
    free(buffer);
}

static SpiceGstVideoBuffer* create_gst_video_buffer(void)
{
    SpiceGstVideoBuffer *buffer = spice_new0(SpiceGstVideoBuffer, 1);
    buffer->base.free = spice_gst_video_buffer_free;
    return buffer;
}


/* ---------- Miscellaneous SpiceGstEncoder helpers ---------- */

static inline double get_mbps(uint64_t bit_rate)
{
    return (double)bit_rate / 1024 / 1024;
}

/* Returns the source frame rate which may change at any time so don't store
 * the result.
 */
static uint32_t get_source_fps(SpiceGstEncoder *encoder)
{
    return encoder->cbs.get_source_fps ?
        encoder->cbs.get_source_fps(encoder->cbs.opaque) : SPICE_GST_DEFAULT_FPS;
}

static uint32_t get_network_latency(SpiceGstEncoder *encoder)
{
    /* Assume that the network latency is symmetric */
    return encoder->cbs.get_roundtrip_ms ?
        encoder->cbs.get_roundtrip_ms(encoder->cbs.opaque) / 2 : 0;
}

static inline int rate_control_is_active(SpiceGstEncoder* encoder)
{
    return encoder->cbs.get_roundtrip_ms != NULL;
}

static inline int is_pipeline_configured(SpiceGstEncoder *encoder)
{
    return encoder->src_caps != NULL;
}

static void free_pipeline(SpiceGstEncoder *encoder)
{
    if (encoder->src_caps) {
        gst_caps_unref(encoder->src_caps);
        encoder->src_caps = NULL;
    }
    if (encoder->pipeline) {
        gst_element_set_state(encoder->pipeline, GST_STATE_NULL);
        gst_object_unref(encoder->appsrc);
        gst_object_unref(encoder->gstenc);
        gst_object_unref(encoder->appsink);
        gst_object_unref(encoder->pipeline);
        encoder->pipeline = NULL;
    }
}


/* ---------- Encoded frame statistics ---------- */

static inline uint32_t get_last_frame_mm_time(SpiceGstEncoder *encoder)
{
    return encoder->history[encoder->history_last].mm_time;
}

/* Returns the current bit rate based on the last
 * SPICE_GST_FRAME_STATISTICS_COUNT frames.
 */
static uint64_t get_effective_bit_rate(SpiceGstEncoder *encoder)
{
    uint32_t next_mm_time = encoder->next_frame_mm_time ?
                            encoder->next_frame_mm_time :
                            get_last_frame_mm_time(encoder) +
                                MSEC_PER_SEC / get_source_fps(encoder);
    uint32_t elapsed = next_mm_time - encoder->history[encoder->stat_first].mm_time;
    return elapsed ? encoder->stat_size_sum * 8 * MSEC_PER_SEC / elapsed : 0;
}

static uint64_t get_average_encoding_time(SpiceGstEncoder *encoder)
{
    uint32_t count = encoder->history_last +
        (encoder->history_last < encoder->stat_first ? SPICE_GST_HISTORY_SIZE : 0) -
        encoder->stat_first + 1;
    return encoder->stat_duration_sum / count;
}

static uint64_t get_average_frame_size(SpiceGstEncoder *encoder)
{
    uint32_t count = encoder->history_last +
        (encoder->history_last < encoder->stat_first ? SPICE_GST_HISTORY_SIZE : 0) -
        encoder->stat_first + 1;
    return encoder->stat_size_sum / count;
}

static uint32_t get_maximum_frame_size(SpiceGstEncoder *encoder)
{
    if (encoder->stat_size_max == 0) {
        uint32_t index = encoder->history_last;
        while (1) {
            encoder->stat_size_max = MAX(encoder->stat_size_max,
                                         encoder->history[index].size);
            if (index == encoder->stat_first) {
                break;
            }
            index = (index ? index : SPICE_GST_HISTORY_SIZE) - 1;
        }
    }
    return encoder->stat_size_max;
}

/* Returns the bit rate of the specified period. from and to must be the
 * mm time of the first and last frame to consider.
 */
static uint64_t get_period_bit_rate(SpiceGstEncoder *encoder, uint32_t from,
                                    uint32_t to)
{
    uint32_t sum = 0;
    uint32_t last_mm_time = 0;
    uint32_t index = encoder->history_last;
    while (1) {
        if (encoder->history[index].mm_time == to) {
            if (last_mm_time == 0) {
                /* We don't know how much time elapsed between the period's
                 * last frame and the next so we cannot include it.
                 */
                sum = 1;
                last_mm_time = to;
            } else {
                sum = encoder->history[index].size + 1;
            }

        } else if (encoder->history[index].mm_time == from) {
            sum += encoder->history[index].size;
            return (sum - 1) * 8 * MSEC_PER_SEC / (last_mm_time - from);

        } else if (index == encoder->history_first) {
            /* This period is outside the recorded history */
            spice_debug("period (%u-%u) outside known history (%u-%u)",
                        from, to,
                        encoder->history[encoder->history_first].mm_time,
                        encoder->history[encoder->history_last].mm_time);
           return 0;

        } else if (sum > 0) {
            sum += encoder->history[index].size;

        } else {
            last_mm_time = encoder->history[index].mm_time;
        }
        index = (index ? index : SPICE_GST_HISTORY_SIZE) - 1;
    }

}

static void add_frame(SpiceGstEncoder *encoder, uint32_t frame_mm_time,
                      uint64_t duration, uint32_t size)
{
    /* Update the statistics */
    uint32_t count = encoder->history_last +
        (encoder->history_last < encoder->stat_first ? SPICE_GST_HISTORY_SIZE : 0) -
        encoder->stat_first + 1;
    if (count == SPICE_GST_FRAME_STATISTICS_COUNT) {
        encoder->stat_duration_sum -= encoder->history[encoder->stat_first].duration;
        encoder->stat_size_sum -= encoder->history[encoder->stat_first].size;
        if (encoder->stat_size_max == encoder->history[encoder->stat_first].size) {
            encoder->stat_size_max = 0;
        }
        encoder->stat_first = (encoder->stat_first + 1) % SPICE_GST_HISTORY_SIZE;
    }
    encoder->stat_duration_sum += duration;
    encoder->stat_size_sum += size;
    if (encoder->stat_size_max > 0 && size > encoder->stat_size_max) {
        encoder->stat_size_max = size;
    }

    /* Update the frame history */
    encoder->history_last = (encoder->history_last + 1) % SPICE_GST_HISTORY_SIZE;
    if (encoder->history_last == encoder->history_first) {
        encoder->history_first = (encoder->history_first + 1) % SPICE_GST_HISTORY_SIZE;
    }
    encoder->history[encoder->history_last].mm_time = frame_mm_time;
    encoder->history[encoder->history_last].duration = duration;
    encoder->history[encoder->history_last].size = size;
}


/* ---------- Encoder bit rate control ---------- */

static void set_video_bit_rate(SpiceGstEncoder *encoder, uint64_t bit_rate)
{
    if (abs(bit_rate - encoder->video_bit_rate) > encoder->video_bit_rate * SPICE_GST_VIDEO_BITRATE_MARGIN) {
        encoder->video_bit_rate = bit_rate;
        if (is_pipeline_configured(encoder)) {
            free_pipeline(encoder);
        }
    }
}

static uint32_t get_min_playback_delay(SpiceGstEncoder *encoder)
{
    /* Make sure the delay is large enough to send a large frame (typically
     * an I frame) and an average frame. This also takes into account the
     * frames dropped by the encoder bit rate control.
     */
    uint32_t size = get_maximum_frame_size(encoder) + get_average_frame_size(encoder);
    uint32_t send_time = MSEC_PER_SEC * size * 8 / encoder->bit_rate;

    /* Also factor in the network latency with a margin for jitter. */
    uint32_t net_latency = get_network_latency(encoder) * (1.0 + SPICE_GST_LATENCY_MARGIN);

    return send_time + net_latency;
}

static void update_client_playback_delay(SpiceGstEncoder *encoder)
{
    if (encoder->cbs.update_client_playback_delay) {
        uint32_t min_delay = get_min_playback_delay(encoder) + get_average_encoding_time(encoder) / NSEC_PER_MILLISEC;
        encoder->cbs.update_client_playback_delay(encoder->cbs.opaque, min_delay);
    }
}

static void update_next_frame_mm_time(SpiceGstEncoder *encoder)
{
    uint64_t period_ns = NSEC_PER_SEC / get_source_fps(encoder);
    uint64_t min_delay_ns = get_average_encoding_time(encoder);
    if (min_delay_ns > period_ns) {
        spice_warning("your system seems to be too slow to encode this %dx%d video in real time", encoder->width, encoder->height);
    }

    min_delay_ns = MIN(min_delay_ns, SPICE_GST_MAX_PERIOD);
    if (encoder->vbuffer_free >= 0) {
        encoder->next_frame_mm_time = get_last_frame_mm_time(encoder) +
                                      min_delay_ns / NSEC_PER_MILLISEC;
        return;
    }

    /* Figure out how many frames to drop to not exceed the current bit rate.
     * Use nanoseconds to avoid precision loss.
     */
    uint64_t delay_ns = -encoder->vbuffer_free * 8 * NSEC_PER_SEC / encoder->bit_rate;
    uint32_t drops = (delay_ns + period_ns - 1) / period_ns; /* round up */
    spice_debug("drops=%u vbuffer %d/%d", drops, encoder->vbuffer_free,
                encoder->vbuffer_size);

    delay_ns = drops * period_ns + period_ns / 2;
    if (delay_ns > SPICE_GST_MAX_PERIOD) {
        /* Reduce the video bit rate so we don't have to drop so many frames. */
        if (encoder->video_bit_rate > encoder->bit_rate * SPICE_GST_BITRATE_MARGIN) {
            set_video_bit_rate(encoder, encoder->bit_rate * SPICE_GST_BITRATE_MARGIN);
        } else {
            set_video_bit_rate(encoder, encoder->bit_rate);
        }
        delay_ns = SPICE_GST_MAX_PERIOD;
    }
    encoder->next_frame_mm_time = get_last_frame_mm_time(encoder) +
                                  MAX(delay_ns, min_delay_ns) / NSEC_PER_MILLISEC;

    /* Drops mean a higher delay between encoded frames so update the
     * playback delay.
     */
    update_client_playback_delay(encoder);
}


/* ---------- Network bit rate control ---------- */

/* The maximum bit rate we will use for the current video.
 *
 * This is based on a 10x compression ratio which should be more than enough
 * for even MJPEG to provide good quality.
 */
static uint64_t get_bit_rate_cap(SpiceGstEncoder *encoder)
{
    uint32_t raw_frame_bits = encoder->width * encoder->height * encoder->format->bpp;
    return raw_frame_bits * get_source_fps(encoder) / 10;
}

static void set_bit_rate(SpiceGstEncoder *encoder, uint64_t bit_rate)
{
    if (bit_rate == 0) {
        /* Use the default value */
        bit_rate = SPICE_GST_DEFAULT_BITRATE;
    }
    if (bit_rate == encoder->bit_rate) {
        return;
    }
    if (bit_rate < SPICE_GST_MIN_BITRATE) {
        /* Don't let the bit rate go too low... */
        encoder->bit_rate = SPICE_GST_MIN_BITRATE;
    } else if (bit_rate > encoder->bit_rate) {
        /* or too high */
        bit_rate = MIN(bit_rate, get_bit_rate_cap(encoder));
    }

    if (bit_rate < encoder->min_bit_rate) {
        encoder->min_bit_rate = bit_rate;
        encoder->bit_rate_step = 0;
    } else if (encoder->status == SPICE_GST_BITRATE_DECREASING &&
               bit_rate > encoder->bit_rate) {
        encoder->min_bit_rate = encoder->bit_rate;
        encoder->bit_rate_step = 0;
    } else if (encoder->status != SPICE_GST_BITRATE_DECREASING &&
               bit_rate < encoder->bit_rate) {
        encoder->max_bit_rate = encoder->bit_rate - SPICE_GST_MIN_BITRATE;
        encoder->bit_rate_step = 0;
    }
    encoder->increase_interval = SPICE_GST_BITRATE_UP_INTERVAL;

    if (encoder->bit_rate_step == 0) {
        encoder->bit_rate_step = MAX(SPICE_GST_MIN_BITRATE,
                                     MIN(SPICE_GST_BITRATE_MAX_STEP,
                                         (encoder->max_bit_rate - encoder->min_bit_rate) / 10));
        encoder->status = (bit_rate < encoder->bit_rate) ? SPICE_GST_BITRATE_DECREASING : SPICE_GST_BITRATE_INCREASING;
        if (encoder->max_bit_rate / SPICE_GST_BITRATE_MARGIN < encoder->min_bit_rate) {
            /* We have sufficiently narrowed down the optimal bit rate range.
             * Settle on the lower end to keep a safety margin and stop rocking
             * the boat.
             */
            bit_rate = encoder->min_bit_rate;
            encoder->status = SPICE_GST_BITRATE_STABLE;
            encoder->increase_interval = encoder->has_client_reports ? SPICE_GST_BITRATE_UP_CLIENT_STABLE : SPICE_GST_BITRATE_UP_SERVER_STABLE;
            set_video_bit_rate(encoder, bit_rate);
        }
    }
    spice_debug("%u set_bit_rate(%.3fMbps) eff %.3f %.3f-%.3f %d",
                get_last_frame_mm_time(encoder) - encoder->last_change,
                get_mbps(bit_rate), get_mbps(get_effective_bit_rate(encoder)),
                get_mbps(encoder->min_bit_rate),
                get_mbps(encoder->max_bit_rate), encoder->status);

    encoder->last_change = get_last_frame_mm_time(encoder);
    encoder->bit_rate = bit_rate;
    /* Adjust the vbuffer size without ever increasing vbuffer_free to avoid
     * sudden bit rate increases.
     */
    int32_t new_size = bit_rate * SPICE_GST_VBUFFER_SIZE / MSEC_PER_SEC / 8;
    if (new_size < encoder->vbuffer_size && encoder->vbuffer_free > 0) {
        encoder->vbuffer_free = MAX(0, encoder->vbuffer_free + new_size - encoder->vbuffer_size);
    }
    encoder->vbuffer_size = new_size;
    update_next_frame_mm_time(encoder);

    /* Frames preceeding the bit rate change are not relevant to the current
     * situation anymore.
     */
    encoder->stat_first = encoder->history_last;
    encoder->stat_duration_sum = encoder->history[encoder->history_last].duration;
    encoder->stat_size_sum = encoder->stat_size_max = encoder->history[encoder->history_last].size;

    if (bit_rate > encoder->video_bit_rate) {
        set_video_bit_rate(encoder, bit_rate * SPICE_GST_BITRATE_MARGIN);
    }
}

static void increase_bit_rate(SpiceGstEncoder *encoder)
{
    if (get_effective_bit_rate(encoder) < encoder->bit_rate) {
        /* The GStreamer encoder currently uses less bandwidth than allowed.
         * So increasing the limit again makes no sense.
         */
        return;
    }

    if (encoder->bit_rate == encoder->max_bit_rate &&
        get_last_frame_mm_time(encoder) - encoder->last_change > SPICE_GST_BITRATE_UP_RESET_MAX) {
        /* The maximum bit rate seems to be sustainable so it was probably set
         * too low. Probe for the maximum bit rate again.
         */
        encoder->max_bit_rate = get_bit_rate_cap(encoder);
        encoder->status = SPICE_GST_BITRATE_INCREASING;
    }

    uint64_t new_bit_rate = MIN(encoder->bit_rate + encoder->bit_rate_step,
                                encoder->max_bit_rate);
    spice_debug("increase bit rate to %.3fMbps %.3f-%.3fMbps %d",
                get_mbps(new_bit_rate), get_mbps(encoder->min_bit_rate),
                get_mbps(encoder->max_bit_rate), encoder->status);
    set_bit_rate(encoder, new_bit_rate);
}


/* ---------- Server feedback ---------- */

/* A helper for gst_encoder_encode_frame()
 *
 * Checks how many frames got dropped since the last encoded frame and adjusts
 * the bit rate accordingly.
 */
static inline gboolean handle_server_drops(SpiceGstEncoder *encoder,
                                           uint32_t frame_mm_time)
{
    if (encoder->server_drops == 0) {
        return FALSE;
    }

    spice_debug("server report: got %u drops in %ums after %ums",
                encoder->server_drops,
                frame_mm_time - get_last_frame_mm_time(encoder),
                frame_mm_time - encoder->last_change);

    /* The server dropped a frame so clearly the buffer is full. */
    encoder->vbuffer_free = MIN(encoder->vbuffer_free, 0);
    /* Add a 0 byte frame so the time spent dropping frames is not counted as
     * time during which the buffer was refilling. This implies dropping this
     * frame.
     */
    add_frame(encoder, frame_mm_time, 0, 0);

    if (encoder->server_drops >= get_source_fps(encoder)) {
        spice_debug("cut the bit rate");
        uint64_t bit_rate = (encoder->bit_rate == encoder->min_bit_rate) ?
            encoder->bit_rate / SPICE_GST_BITRATE_CUT :
            MAX(encoder->min_bit_rate, encoder->bit_rate / SPICE_GST_BITRATE_CUT);
        set_bit_rate(encoder, bit_rate);

    } else {
        spice_debug("reduce the bit rate");
        uint64_t bit_rate = (encoder->bit_rate == encoder->min_bit_rate) ?
            encoder->bit_rate / SPICE_GST_BITRATE_REDUCE :
            MAX(encoder->min_bit_rate, encoder->bit_rate / SPICE_GST_BITRATE_REDUCE);
        set_bit_rate(encoder, bit_rate);
    }
    encoder->server_drops = 0;
    return TRUE;
}

/* A helper for gst_encoder_encode_frame() */
static inline void server_increase_bit_rate(SpiceGstEncoder *encoder,
                                            uint32_t frame_mm_time)
{
    /* Let gst_encoder_client_stream_report() deal with bit rate increases if
     * we receive client reports.
     */
    if (!encoder->has_client_reports && encoder->server_drops == 0 &&
        frame_mm_time - encoder->last_change >= encoder->increase_interval) {
        increase_bit_rate(encoder);
    }
}


/* ---------- GStreamer pipeline ---------- */

/* A helper for spice_gst_encoder_encode_frame() */
static const SpiceFormatForGStreamer *map_format(SpiceBitmapFmt format)
{
    /* See GStreamer's part-mediatype-video-raw.txt and
     * section-types-definitions.html documents.
     */
    static const SpiceFormatForGStreamer format_map[] =  {
        {SPICE_BITMAP_FMT_RGBA, "BGRA", 32},
        {SPICE_BITMAP_FMT_16BIT, "RGB15", 16},
        /* TODO: Test the other formats */
        {SPICE_BITMAP_FMT_32BIT, "BGRx", 32},
        {SPICE_BITMAP_FMT_24BIT, "BGR", 24},
    };

    int i;
    for (i = 0; i < G_N_ELEMENTS(format_map); i++) {
        if (format_map[i].spice_format == format) {
            if (i > 1) {
                spice_warning("The %d format has not been tested yet", format);
            }
            return &format_map[i];
        }
    }

    return NULL;
}

static void set_appsrc_caps(SpiceGstEncoder *encoder)
{
    if (encoder->src_caps) {
        gst_caps_unref(encoder->src_caps);
    }
    encoder->src_caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, encoder->format->format,
        "width", G_TYPE_INT, encoder->width,
        "height", G_TYPE_INT, encoder->height,
        "framerate", GST_TYPE_FRACTION, get_source_fps(encoder), 1,
        NULL);
    g_object_set(G_OBJECT(encoder->appsrc), "caps", encoder->src_caps, NULL);
}

static int physical_core_count = 0;
static int get_physical_core_count(void)
{
    if (!physical_core_count) {
#ifdef HAVE_G_GET_NUMPROCESSORS
        physical_core_count = g_get_num_processors();
#endif
        if (system("egrep -l '^flags\\b.*: .*\\bht\\b' /proc/cpuinfo >/dev/null 2>&1") == 0) {
            /* Hyperthreading is enabled so divide by two to get the number
             * of physical cores.
             */
            physical_core_count = physical_core_count / 2;
        }
        if (physical_core_count == 0)
            physical_core_count = 1;
    }
    return physical_core_count;
}

static const gchar* get_gst_codec_name(SpiceGstEncoder *encoder)
{
    switch (encoder->base.codec_type)
    {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        return "avenc_mjpeg";
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        return "vp8enc";
    case SPICE_VIDEO_CODEC_TYPE_H264:
        return "x264enc";
    default:
        /* gstreamer_encoder_new() should have rejected this codec type */
        spice_warning("unsupported codec type %d", encoder->base.codec_type);
        return NULL;
    }
}

static gboolean create_pipeline(SpiceGstEncoder *encoder)
{
    const gchar* gstenc_name = get_gst_codec_name(encoder);
    if (!gstenc_name) {
        return FALSE;
    }
    gchar* gstenc_opts;
    switch (encoder->base.codec_type)
    {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        /* Set max-threads to ensure zero-frame latency */
        gstenc_opts = g_strdup("max-threads=1");
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8: {
        /* See http://www.webmproject.org/docs/encoder-parameters/
         * - Set end-usage to get a constant bitrate to help with streaming.
         * - resize-allowed allows trading resolution for low bitrates while
         *   min-quantizer ensures the bitrate does not get needlessly high.
         * - error-resilient minimises artifacts in case the client drops a
         *   frame.
         * - Set lag-in-frames, deadline and cpu-used to match
         *   "Profile Realtime". lag-in-frames ensures zero-frame latency,
         *   deadline turns on realtime behavior, and cpu-used targets a 75%
         *   CPU usage.
         * - deadline is supposed to be set in microseconds but in practice
         *   it behaves like a boolean.
         * - At least up to GStreamer 1.6.2, vp8enc cannot be trusted to pick
         *   the optimal number of threads. Also exceeding the number of
         *   physical core really degrades image quality.
         * - token-partitions parallelizes more operations.
         */
        int threads = get_physical_core_count();
        int parts = threads < 2 ? 0 : threads < 4 ? 1 : threads < 8 ? 2 : 3;
        gstenc_opts = g_strdup_printf("end-usage=cbr min-quantizer=10 resize-allowed=true error-resilient=true lag-in-frames=0 deadline=1 cpu-used=4 threads=%d token-partitions=%d", threads, parts);
        break;
        }
    case SPICE_VIDEO_CODEC_TYPE_H264:
        /* - Set tune and sliced-threads to ensure a zero-frame latency
         * - qp-min ensures the bitrate does not get needlessly high.
         * - Set speed-preset to get realtime speed.
         * - Set intra-refresh to get more uniform compressed frame sizes,
         *   thus helping with streaming.
         */
        gstenc_opts = g_strdup("byte-stream=true aud=true qp-min=15 tune=4 sliced-threads=true speed-preset=ultrafast intra-refresh=true");
        break;
    default:
        /* gstreamer_encoder_new() should have rejected this codec type */
        spice_warning("unsupported codec type %d", encoder->base.codec_type);
        return FALSE;
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc is-live=true format=time do-timestamp=true name=src ! videoconvert ! %s %s name=encoder ! appsink name=sink", gstenc_name, gstenc_opts);
    spice_debug("GStreamer pipeline: %s", desc);
    encoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(gstenc_opts);
    g_free(desc);
    if (!encoder->pipeline || err) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        if (encoder->pipeline) {
            gst_object_unref(encoder->pipeline);
            encoder->pipeline = NULL;
        }
        return FALSE;
    }
    encoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "src"));
    encoder->gstenc = gst_bin_get_by_name(GST_BIN(encoder->pipeline), "encoder");
    encoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "sink"));
    return TRUE;
}

/* A helper for spice_gst_encoder_encode_frame() */
static gboolean configure_pipeline(SpiceGstEncoder *encoder,
                                   const SpiceBitmap *bitmap)
{
    if (!encoder->pipeline && !create_pipeline(encoder)) {
        return FALSE;
    }

    /* Configure the encoder bitrate */
    switch (encoder->base.codec_type) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        g_object_set(G_OBJECT(encoder->gstenc),
                     "bitrate", (gint)encoder->video_bit_rate,
                     NULL);
        /* See https://bugzilla.gnome.org/show_bug.cgi?id=753257 */
        spice_debug("removing the pipeline clock");
        gst_pipeline_use_clock(GST_PIPELINE(encoder->pipeline), NULL);
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        g_object_set(G_OBJECT(encoder->gstenc),
                     "target-bitrate", (gint)encoder->video_bit_rate,
                     NULL);
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        g_object_set(G_OBJECT(encoder->gstenc),
                     "bitrate", (guint)(encoder->bit_rate / 1024),
                     NULL);
        break;
    default:
        /* gstreamer_encoder_new() should have rejected this codec type */
        spice_warning("unsupported codec type %d", encoder->base.codec_type);
        free_pipeline(encoder);
        return FALSE;
    }

    /* Set the source caps */
    set_appsrc_caps(encoder);

    /* Start playing */
    if (gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_warning("GStreamer error: unable to set the pipeline to the playing state");
        free_pipeline(encoder);
        return FALSE;
    }

    return TRUE;
}

/* A helper for spice_gst_encoder_encode_frame() */
static void reconfigure_pipeline(SpiceGstEncoder *encoder)
{
    if (!is_pipeline_configured(encoder)) {
        return;
    }
    if (encoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_VP8) {
        /* vp8enc fails to account for caps changes that modify the frame
         * size and complains about the buffer size.
         * So recreate the pipeline from scratch.
         */
        free_pipeline(encoder);
        return;
    }

    if (gst_element_set_state(encoder->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        spice_debug("GStreamer error: could not pause the pipeline, rebuilding it instead");
        free_pipeline(encoder);
    }
    set_appsrc_caps(encoder);
    if (gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_debug("GStreamer error: could not restart the pipeline, rebuilding it instead");
        free_pipeline(encoder);
    }
}

/* A helper for the *_copy() functions */
static int is_chunk_padded(const SpiceBitmap *bitmap, uint32_t index)
{
    SpiceChunks *chunks = bitmap->data;
    if (chunks->chunk[index].len % bitmap->stride != 0) {
        spice_warning("chunk %d/%d is padded, cannot copy", index, chunks->num_chunks);
        return TRUE;
    }
    return FALSE;
}

/* A helper for push_raw_frame() */
static inline int line_copy(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                            uint32_t chunk_offset, uint32_t stream_stride,
                            uint32_t height, uint8_t *buffer)
{
     uint8_t *dst = buffer;
     SpiceChunks *chunks = bitmap->data;
     uint32_t chunk_index = 0;
     for (int l = 0; l < height; l++) {
         /* We may have to move forward by more than one chunk the first
          * time around.
          */
         while (chunk_offset >= chunks->chunk[chunk_index].len) {
             if (is_chunk_padded(bitmap, chunk_index)) {
                 return FALSE;
             }
             chunk_offset -= chunks->chunk[chunk_index].len;
             chunk_index++;
         }

         /* Copy the line */
         uint8_t *src = chunks->chunk[chunk_index].data + chunk_offset;
         memcpy(dst, src, stream_stride);
         dst += stream_stride;
         chunk_offset += bitmap->stride;
     }
     spice_return_val_if_fail(dst - buffer == stream_stride * height, FALSE);
     return TRUE;
}

#ifdef DO_ZERO_COPY
typedef struct {
    gint refs;
    SpiceGstEncoder *encoder;
    gpointer opaque;
} BitmapWrapper;

static void clear_zero_copy_queue(SpiceGstEncoder *encoder, gboolean unref_queue)
{
    gpointer bitmap_opaque;
    while ((bitmap_opaque = g_async_queue_try_pop(encoder->unused_bitmap_opaques))) {
        encoder->bitmap_unref(bitmap_opaque);
    }
    if (unref_queue) {
        g_async_queue_unref(encoder->unused_bitmap_opaques);
    }
}

static BitmapWrapper *bitmap_wrapper_new(SpiceGstEncoder *encoder, gpointer bitmap_opaque)
{
    BitmapWrapper *wrapper = spice_new(BitmapWrapper, 1);
    wrapper->refs = 1;
    wrapper->encoder = encoder;
    wrapper->opaque = bitmap_opaque;
    encoder->bitmap_ref(bitmap_opaque);
    return wrapper;
}

static void bitmap_wrapper_unref(gpointer data)
{
    BitmapWrapper *wrapper = data;
    if (g_atomic_int_dec_and_test(&wrapper->refs)) {
        g_async_queue_push(wrapper->encoder->unused_bitmap_opaques, wrapper->opaque);
        free(wrapper);
    }
}


/* A helper for push_raw_frame() */
static inline int zero_copy(SpiceGstEncoder *encoder,
                            const SpiceBitmap *bitmap, gpointer bitmap_opaque,
                            GstBuffer *buffer, uint32_t *chunk_index,
                            uint32_t *chunk_offset, uint32_t *len)
{
    const SpiceChunks *chunks = bitmap->data;
    while (*chunk_index < chunks->num_chunks &&
           *chunk_offset >= chunks->chunk[*chunk_index].len) {
        if (is_chunk_padded(bitmap, *chunk_index)) {
            return FALSE;
        }
        *chunk_offset -= chunks->chunk[*chunk_index].len;
        (*chunk_index)++;
    }

    int max_mem = gst_buffer_get_max_memory();
    if (chunks->num_chunks - *chunk_index > max_mem) {
        /* There are more chunks than we can fit memory objects in a
         * buffer. This will cause the buffer to merge memory objects to
         * fit the extra chunks, which means doing wasteful memory copies.
         * So use the zero-copy approach for the first max_mem-1 chunks, and
         * let push_raw_frame() deal with the rest.
         */
        max_mem = *chunk_index + max_mem - 1;
    } else {
        max_mem = chunks->num_chunks;
    }

    BitmapWrapper *wrapper = NULL;
    while (*len && *chunk_index < max_mem) {
        if (is_chunk_padded(bitmap, *chunk_index)) {
            return FALSE;
        }
        if (wrapper) {
            wrapper->refs++;
        } else {
            wrapper = bitmap_wrapper_new(encoder, bitmap_opaque);
        }
        uint32_t thislen = MIN(chunks->chunk[*chunk_index].len - *chunk_offset, *len);
        GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY,
                                                chunks->chunk[*chunk_index].data,
                                                chunks->chunk[*chunk_index].len,
                                                *chunk_offset, thislen,
                                                wrapper, bitmap_wrapper_unref);
        gst_buffer_append_memory(buffer, mem);
        *len -= thislen;
        *chunk_offset = 0;
        (*chunk_index)++;
    }
    return TRUE;
}
#else
static void clear_zero_copy_queue(SpiceGstEncoder *encoder, gboolean unref_queue)
{
    /* Nothing to do */
}
#endif

/* A helper for push_raw_frame() */
static inline int chunk_copy(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                             uint32_t chunk_index, uint32_t chunk_offset,
                             uint32_t len, uint8_t *dst)
{
    SpiceChunks *chunks = bitmap->data;
    /* Skip chunks until we find the start of the frame */
    while (chunk_index < chunks->num_chunks &&
           chunk_offset >= chunks->chunk[chunk_index].len) {
        if (is_chunk_padded(bitmap, chunk_index)) {
            return FALSE;
        }
        chunk_offset -= chunks->chunk[chunk_index].len;
        chunk_index++;
    }

    /* We can copy the frame chunk by chunk */
    while (len && chunk_index < chunks->num_chunks) {
        if (is_chunk_padded(bitmap, chunk_index)) {
            return FALSE;
        }
        uint8_t *src = chunks->chunk[chunk_index].data + chunk_offset;
        uint32_t thislen = MIN(chunks->chunk[chunk_index].len - chunk_offset, len);
        memcpy(dst, src, thislen);
        dst += thislen;
        len -= thislen;
        chunk_offset = 0;
        chunk_index++;
    }
    spice_return_val_if_fail(len == 0, FALSE);
    return TRUE;
}

/* A helper for push_raw_frame() */
static uint8_t *allocate_and_map_memory(gsize size, GstMapInfo *map, GstBuffer *buffer)
{
    GstMemory *mem = gst_allocator_alloc(NULL, size, NULL);
    if (!mem) {
        gst_buffer_unref(buffer);
        return NULL;
    }
    if (!gst_memory_map(mem, map, GST_MAP_WRITE))
    {
        gst_memory_unref(mem);
        gst_buffer_unref(buffer);
        return NULL;
    }
    return map->data;
}

/* A helper for spice_gst_encoder_encode_frame() */
static int push_raw_frame(SpiceGstEncoder *encoder,
                          const SpiceBitmap *bitmap,
                          const SpiceRect *src, int top_down,
                          gpointer bitmap_opaque)
{
    uint32_t height = src->bottom - src->top;
    uint32_t stream_stride = (src->right - src->left) * encoder->format->bpp / 8;
    uint32_t len = stream_stride * height;
    GstBuffer *buffer = gst_buffer_new();
    /* TODO Use GST_MAP_INFO_INIT once GStreamer 1.4.5 is no longer relevant */
    GstMapInfo map = { .memory = NULL };

    /* Note that we should not reorder the lines, even if top_down is false.
     * It just changes the number of lines to skip at the start of the bitmap.
     */
    uint32_t skip_lines = top_down ? src->top : bitmap->y - (src->bottom - 0);
    uint32_t chunk_offset = bitmap->stride * skip_lines;

    if (stream_stride != bitmap->stride) {
        /* We have to do a line-by-line copy because for each we have to
         * leave out pixels on the left or right.
         */
        uint8_t *dst = allocate_and_map_memory(len, &map, buffer);
        if (!dst) {
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }

        chunk_offset += src->left * encoder->format->bpp / 8;
        if (!line_copy(encoder, bitmap, chunk_offset, stream_stride, height, dst)) {
            gst_memory_unmap(map.memory, &map);
            gst_memory_unref(map.memory);
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
    } else {
        uint32_t chunk_index = 0;
        /* We can copy the bitmap chunk by chunk */
#ifdef DO_ZERO_COPY
        if (!zero_copy(encoder, bitmap, bitmap_opaque, buffer, &chunk_index, &chunk_offset, &len)) {
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        /* len now contains the remaining number of bytes to copy.
         * However we must avoid any write to the GstBuffer object as it
         * would cause a copy of the read-only memory objects we just added.
         * Fortunately we can append extra writable memory objects instead.
         */
#endif

        if (len) {
            uint8_t *dst = allocate_and_map_memory(len, &map, buffer);
            if (!dst) {
                return VIDEO_ENCODER_FRAME_UNSUPPORTED;
            }
            if (!chunk_copy(encoder, bitmap, chunk_index, chunk_offset, len, dst)) {
                gst_memory_unmap(map.memory, &map);
                gst_memory_unref(map.memory);
                gst_buffer_unref(buffer);
                return VIDEO_ENCODER_FRAME_UNSUPPORTED;
            }
        }
    }
    if (map.memory) {
        gst_memory_unmap(map.memory, &map);
        gst_buffer_append_memory(buffer, map.memory);
    }
    GST_BUFFER_OFFSET(buffer) = encoder->frame++;

    GstFlowReturn ret = gst_app_src_push_buffer(encoder->appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        spice_warning("GStreamer error: unable to push source buffer (%d)", ret);
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    return VIDEO_ENCODER_FRAME_ENCODE_DONE;
}

/* A helper for spice_gst_encoder_encode_frame() */
static int pull_compressed_buffer(SpiceGstEncoder *encoder,
                                  VideoBuffer **outbuf)
{
    GstSample *sample = gst_app_sink_pull_sample(encoder->appsink);
    if (sample) {
        SpiceGstVideoBuffer *buffer = create_gst_video_buffer();
        buffer->gst_buffer = gst_sample_get_buffer(sample);
        if (buffer->gst_buffer &&
            gst_buffer_map(buffer->gst_buffer, &buffer->map, GST_MAP_READ)) {
            buffer->base.data = buffer->map.data;
            buffer->base.size = gst_buffer_get_size(buffer->gst_buffer);
            *outbuf = (VideoBuffer*)buffer;
            gst_buffer_ref(buffer->gst_buffer);
            gst_sample_unref(sample);
            return VIDEO_ENCODER_FRAME_ENCODE_DONE;
        }
        buffer->base.free((VideoBuffer*)buffer);
        gst_sample_unref(sample);
    }
    spice_debug("failed to pull the compressed buffer");
    return VIDEO_ENCODER_FRAME_UNSUPPORTED;
}


/* ---------- VideoEncoder's public API ---------- */

static void spice_gst_encoder_destroy(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    free_pipeline(encoder);
    /* Unref any lingering bitmap opaque structures from past frames */
    clear_zero_copy_queue(encoder, TRUE);
    free(encoder);
}

static int spice_gst_encoder_encode_frame(VideoEncoder *video_encoder,
                                          uint32_t frame_mm_time,
                                          const SpiceBitmap *bitmap,
                                          int width, int height,
                                          const SpiceRect *src, int top_down,
                                          gpointer bitmap_opaque,
                                          VideoBuffer **outbuf)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    g_return_val_if_fail(outbuf != NULL, VIDEO_ENCODER_FRAME_UNSUPPORTED);
    *outbuf = NULL;

    /* Unref the last frame's bitmap_opaque structures if any */
    clear_zero_copy_queue(encoder, FALSE);

    if (width != encoder->width || height != encoder->height ||
        encoder->spice_format != bitmap->format) {
        spice_debug("video format change: width %d -> %d, height %d -> %d, format %d -> %d",
                    encoder->width, width, encoder->height, height,
                    encoder->spice_format, bitmap->format);
        encoder->format = map_format(bitmap->format);
        if (!encoder->format) {
            spice_warning("unable to map format type %d", bitmap->format);
            encoder->errors = 4;
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        encoder->spice_format = bitmap->format;
        encoder->width = width;
        encoder->height = height;
        if (encoder->bit_rate == 0) {
            encoder->history[0].mm_time = frame_mm_time;
            encoder->max_bit_rate = get_bit_rate_cap(encoder);
            encoder->min_bit_rate = SPICE_GST_MIN_BITRATE;
            encoder->status = SPICE_GST_BITRATE_DECREASING;
            set_bit_rate(encoder, encoder->starting_bit_rate);
            encoder->vbuffer_free = 0; /* Slow start */
        } else if (encoder->pipeline) {
            reconfigure_pipeline(encoder);
        }
        encoder->errors = 0;
    } else if (encoder->errors >= 3) {
        /* The pipeline keeps failing to handle the frames we send it, which
         * is usually because they are too small (mouse pointer-sized).
         * So give up until something changes.
         */
        if (encoder->errors == 3) {
            spice_debug("%s cannot compress %dx%d:%dbpp frames",
                        get_gst_codec_name(encoder), encoder->width,
                        encoder->height, encoder->format->bpp);
            encoder->errors++;
        }
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    if (rate_control_is_active(encoder) &&
        (handle_server_drops(encoder, frame_mm_time) ||
         frame_mm_time < encoder->next_frame_mm_time)) {
        /* Drop the frame to limit the outgoing bit rate. */
        return VIDEO_ENCODER_FRAME_DROP;
    }

    if (!is_pipeline_configured(encoder) &&
        !configure_pipeline(encoder, bitmap)) {
        encoder->errors++;
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    uint64_t start = spice_get_monotonic_time_ns();
    int rc = push_raw_frame(encoder, bitmap, src, top_down, bitmap_opaque);
    if (rc == VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        rc = pull_compressed_buffer(encoder, outbuf);
        if (rc != VIDEO_ENCODER_FRAME_ENCODE_DONE) {
            /* The input buffer will be stuck in the pipeline, preventing
             * later ones from being processed. So reset the pipeline.
             */
            free_pipeline(encoder);
            encoder->errors++;
        }
    }
    /* Unref the last frame's bitmap_opaque structures if any */
    clear_zero_copy_queue(encoder, FALSE);

    if (rc != VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        return rc;
    }
    uint32_t last_mm_time = get_last_frame_mm_time(encoder);
    add_frame(encoder, frame_mm_time, spice_get_monotonic_time_ns() - start,
              (*outbuf)->size);

    int32_t refill = encoder->bit_rate * (frame_mm_time - last_mm_time) / MSEC_PER_SEC / 8;
    encoder->vbuffer_free = MIN(encoder->vbuffer_free + refill,
                                encoder->vbuffer_size) - (*outbuf)->size;

    server_increase_bit_rate(encoder, frame_mm_time);
    update_next_frame_mm_time(encoder);

    return rc;
}

static void spice_gst_encoder_client_stream_report(VideoEncoder *video_encoder,
                                             uint32_t num_frames,
                                             uint32_t num_drops,
                                             uint32_t start_frame_mm_time,
                                             uint32_t end_frame_mm_time,
                                             int32_t video_margin,
                                             uint32_t audio_margin)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    encoder->has_client_reports = TRUE;

    encoder->max_video_margin = MAX(encoder->max_video_margin, video_margin);
    encoder->max_audio_margin = MAX(encoder->max_audio_margin, audio_margin);
    int32_t margin_delta = video_margin - encoder->last_video_margin;
    encoder->last_video_margin = video_margin;

    uint64_t period_bit_rate = get_period_bit_rate(encoder, start_frame_mm_time, end_frame_mm_time);
    spice_debug("client report: %u/%u drops in %ums margins video %3d/%3d audio %3u/%3u bw %.3f/%.3fMbps%s",
                num_drops, num_frames, end_frame_mm_time - start_frame_mm_time,
                video_margin, encoder->max_video_margin,
                audio_margin, encoder->max_audio_margin,
                get_mbps(period_bit_rate),
                get_mbps(get_effective_bit_rate(encoder)),
                start_frame_mm_time < encoder->last_change ? " obsolete" : "");
    if (encoder->status == SPICE_GST_BITRATE_DECREASING &&
        start_frame_mm_time < encoder->last_change) {
        /* Some of this data predates the last bit rate reduction
         * so it is obsolete.
         */
        return;
    }

    /* We normally arrange for even the largest frames to arrive a bit over
     * one period before they should be displayed.
     */
    uint32_t min_margin = MSEC_PER_SEC / get_source_fps(encoder) +
        get_network_latency(encoder) * SPICE_GST_LATENCY_MARGIN;

    /* A low video margin indicates that the bit rate is too high. */
    uint32_t score;
    if (num_drops) {
        score = 4;
    } else if (margin_delta >= 0) {
        /* The situation was bad but seems to be improving */
        score = 0;
    } else if (video_margin < min_margin * SPICE_GST_VIDEO_MARGIN_BAD ||
               video_margin < encoder->max_video_margin * SPICE_GST_VIDEO_MARGIN_BAD) {
        score = 3;
    } else if (video_margin < min_margin ||
               video_margin < encoder->max_video_margin * SPICE_GST_VIDEO_MARGIN_AVERAGE) {
        score = 2;
    } else if (video_margin < encoder->max_video_margin * SPICE_GST_VIDEO_MARGIN_GOOD) {
        score = 1;
    } else {
        score = 0;
    }
    /* A fast dropping video margin is a compounding factor. */
    if (margin_delta < -abs(encoder->max_video_margin) * SPICE_GST_VIDEO_DELTA_BAD) {
        score += 2;
    } else if (margin_delta < -abs(encoder->max_video_margin) * SPICE_GST_VIDEO_DELTA_AVERAGE) {
        score += 1;
    }

    if (score > 3) {
        spice_debug("score %u, cut the bit rate", score);
        uint64_t bit_rate = (encoder->bit_rate == encoder->min_bit_rate) ?
            encoder->bit_rate / SPICE_GST_BITRATE_CUT :
            MAX(encoder->min_bit_rate, encoder->bit_rate / SPICE_GST_BITRATE_CUT);
        set_bit_rate(encoder, bit_rate);

    } else if (score == 3) {
        spice_debug("score %u, reduce the bit rate", score);
        uint64_t bit_rate = (encoder->bit_rate == encoder->min_bit_rate) ?
            encoder->bit_rate / SPICE_GST_BITRATE_REDUCE :
            MAX(encoder->min_bit_rate, encoder->bit_rate / SPICE_GST_BITRATE_REDUCE);
        set_bit_rate(encoder, bit_rate);

    } else if (score == 2) {
        spice_debug("score %u, decrement the bit rate", score);
        set_bit_rate(encoder, encoder->bit_rate - encoder->bit_rate_step);

    } else if (audio_margin < encoder->max_audio_margin * SPICE_GST_AUDIO_MARGIN_BAD &&
               audio_margin * SPICE_GST_AUDIO_VIDEO_RATIO < video_margin) {
        /* The audio margin has decreased a lot while the video_margin
         * remained higher. It may be that the video stream is starving the
         * audio one of bandwidth. So reduce the bit rate.
         */
        spice_debug("free some bandwidth for the audio stream");
        set_bit_rate(encoder, encoder->bit_rate - encoder->bit_rate_step);

    } else if (score == 1 && period_bit_rate <= encoder->bit_rate &&
               encoder->status == SPICE_GST_BITRATE_INCREASING) {
        /* We only increase the bit rate when score == 0 so things got worse
         * since the last increase, and not because of a transient bit rate
         * peak.
         */
        spice_debug("degraded margin, decrement bit rate %.3f <= %.3fMbps",
                    get_mbps(period_bit_rate), get_mbps(encoder->bit_rate));
        set_bit_rate(encoder, encoder->bit_rate - encoder->bit_rate_step);

    } else if (score == 0 &&
               get_last_frame_mm_time(encoder) - encoder->last_change >= encoder->increase_interval) {
        /* The video margin is consistently high so increase the bit rate. */
        increase_bit_rate(encoder);
    }
}

static void spice_gst_encoder_notify_server_frame_drop(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    if (encoder->server_drops == 0) {
        spice_debug("server report: getting frame drops...");
    }
    encoder->server_drops++;
}

static uint64_t spice_gst_encoder_get_bit_rate(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    return get_effective_bit_rate(encoder);
}

static void spice_gst_encoder_get_stats(VideoEncoder *video_encoder,
                                        VideoEncoderStats *stats)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    uint64_t raw_bit_rate = encoder->width * encoder->height * (encoder->format ? encoder->format->bpp : 0) * get_source_fps(encoder);

    spice_return_if_fail(stats != NULL);
    stats->starting_bit_rate = encoder->starting_bit_rate;
    stats->cur_bit_rate = get_effective_bit_rate(encoder);

    /* Use the compression level as a proxy for the quality */
    stats->avg_quality = stats->cur_bit_rate ? 100.0 - raw_bit_rate / stats->cur_bit_rate : 0;
    if (stats->avg_quality < 0) {
        stats->avg_quality = 0;
    }
}

VideoEncoder *gstreamer_encoder_new(SpiceVideoCodecType codec_type,
                                    uint64_t starting_bit_rate,
                                    VideoEncoderRateControlCbs *cbs,
                                    bitmap_ref_t bitmap_ref,
                                    bitmap_unref_t bitmap_unref)
{
    spice_return_val_if_fail(SPICE_GST_FRAME_STATISTICS_COUNT <= SPICE_GST_HISTORY_SIZE, NULL);
    spice_return_val_if_fail(codec_type == SPICE_VIDEO_CODEC_TYPE_MJPEG ||
                             codec_type == SPICE_VIDEO_CODEC_TYPE_VP8 ||
                             codec_type == SPICE_VIDEO_CODEC_TYPE_H264, NULL);

    GError *err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return NULL;
    }

    SpiceGstEncoder *encoder = spice_new0(SpiceGstEncoder, 1);
    encoder->base.destroy = spice_gst_encoder_destroy;
    encoder->base.encode_frame = spice_gst_encoder_encode_frame;
    encoder->base.client_stream_report = spice_gst_encoder_client_stream_report;
    encoder->base.notify_server_frame_drop = spice_gst_encoder_notify_server_frame_drop;
    encoder->base.get_bit_rate = spice_gst_encoder_get_bit_rate;
    encoder->base.get_stats = spice_gst_encoder_get_stats;
    encoder->base.codec_type = codec_type;
#ifdef DO_ZERO_COPY
    encoder->unused_bitmap_opaques = g_async_queue_new();
#endif

    if (cbs) {
        encoder->cbs = *cbs;
    }
    encoder->starting_bit_rate = starting_bit_rate;
    encoder->bitmap_ref = bitmap_ref;
    encoder->bitmap_unref = bitmap_unref;

    /* All the other fields are initialized to zero by spice_new0(). */

    if (!create_pipeline(encoder)) {
        /* Some GStreamer dependency is probably missing */
        free(encoder);
        encoder = NULL;
    }
    return (VideoEncoder*)encoder;
}
