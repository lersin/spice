/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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

/* Replay a previously recorded file (via SPICE_WORKER_RECORD_FILENAME)
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>

#include <spice/macros.h>
#include "test-display-base.h"
#include <common/log.h>

static SpiceCoreInterface *core;
static SpiceServer *server;
static SpiceReplay *replay;
static QXLWorker *qxl_worker = NULL;
static gboolean started = FALSE;
static QXLInstance display_sin = { {0}, 0, NULL };
static gint slow = 0;
static gint skip = 0;
static gboolean print_count = FALSE;
static guint ncommands = 0;
static pid_t client_pid;
static GMainLoop *loop = NULL;
static GAsyncQueue *display_queue = NULL;
static GAsyncQueue *cursor_queue = NULL;
static long total_size;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static GSource *fill_source = NULL;


#define MEM_SLOT_GROUP_ID 0

/* Parts cribbed from spice-display.h/.c/qxl.c */

static QXLDevMemSlot slot = {
.slot_group_id = MEM_SLOT_GROUP_ID,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

static void attach_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
{
    static int count = 0;
    if (++count > 1) {
        g_warning("%s ignored\n", __func__);
        return;
    }
    g_debug("%s\n", __func__);
    qxl_worker = _qxl_worker;
    spice_qxl_add_memslot(qin, &slot);
    spice_server_vm_start(server);
}

static void set_compression_level(QXLInstance *qin, int level)
{
    g_debug("%s\n", __func__);
}

static void set_mm_time(QXLInstance *qin, uint32_t mm_time)
{
}

// same as qemu/ui/spice-display.h
#define MAX_SURFACE_NUM 1024

static void get_init_info(QXLInstance *qin, QXLDevInitInfo *info)
{
    bzero(info, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = MAX_SURFACE_NUM;
}

static gboolean fill_queue_idle(gpointer user_data)
{
    gboolean keep = FALSE;
    gboolean wakeup = FALSE;

    while ((g_async_queue_length(display_queue) +
            g_async_queue_length(cursor_queue)) < 50) {
        QXLCommandExt *cmd = spice_replay_next_cmd(replay, qxl_worker);
        if (!cmd) {
            g_async_queue_push(display_queue, GINT_TO_POINTER(-1));
            g_async_queue_push(cursor_queue, GINT_TO_POINTER(-1));
            goto end;
        }

        ++ncommands;

        if (slow && (ncommands > skip)) {
            g_usleep(slow);
        }

        wakeup = TRUE;
        if (cmd->cmd.type == QXL_CMD_CURSOR) {
            g_async_queue_push(cursor_queue, cmd);
        } else {
            g_async_queue_push(display_queue, cmd);
        }
    }

end:
    if (!keep) {
        pthread_mutex_lock(&mutex);
        if (fill_source) {
            g_source_destroy(fill_source);
            g_source_unref(fill_source);
            fill_source = NULL;
        }
        pthread_mutex_unlock(&mutex);
    }
    if (wakeup)
        spice_qxl_wakeup(&display_sin);

    return keep;
}

static void fill_queue(void)
{
    pthread_mutex_lock(&mutex);

    if (!started)
        goto end;

    if (fill_source)
        goto end;

    fill_source = g_idle_source_new();
    g_source_set_callback(fill_source, fill_queue_idle, NULL, NULL);
    g_source_attach(fill_source, basic_event_loop_get_context());

end:
    pthread_mutex_unlock(&mutex);
}


// called from spice_server thread (i.e. red_worker thread)
static int get_command_from(QXLInstance *qin, QXLCommandExt *ext, GAsyncQueue *queue)
{
    QXLCommandExt *cmd;

    if (g_async_queue_length(queue) == 0) {
        /* could use a gcondition ? */
        fill_queue();
        return FALSE;
    }

    cmd = g_async_queue_try_pop(queue);
    if (GPOINTER_TO_INT(cmd) == -1) {
        g_main_loop_quit(loop);
        return FALSE;
    }

    *ext = *cmd;

    return TRUE;
}

static int req_notification(GAsyncQueue *queue)
{
    /* check for pending messages */
    return g_async_queue_length(queue) == 0;
}

static int get_display_command(QXLInstance *qin, QXLCommandExt *ext)
{
    return get_command_from(qin, ext, display_queue);
}

static int req_display_notification(QXLInstance *qin)
{
    return req_notification(display_queue);
}

static void end_replay(void)
{
    int child_status;

    /* FIXME: wait threads and end cleanly */
    spice_replay_free(replay);

    if (client_pid) {
        g_debug("kill %d", client_pid);
        kill(client_pid, SIGINT);
        waitpid(client_pid, &child_status, 0);
    }
}

static void release_resource(QXLInstance *qin, struct QXLReleaseInfoExt release_info)
{
    spice_replay_free_cmd(replay, (QXLCommandExt *)release_info.info->id);
}

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    return get_command_from(qin, ext, cursor_queue);
}

static int req_cursor_notification(QXLInstance *qin)
{
    return req_notification(cursor_queue);
}

static void notify_update(QXLInstance *qin, uint32_t update_id)
{
}

static int flush_resources(QXLInstance *qin)
{
    return TRUE;
}

static QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "replay",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attach_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,
    .get_command = get_display_command,
    .req_cmd_notification = req_display_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
};

static void replay_channel_event(int event, SpiceChannelEventInfo *info)
{
    if (info->type == SPICE_CHANNEL_DISPLAY &&
        event == SPICE_CHANNEL_EVENT_INITIALIZED) {
        started = TRUE;
    }
}

static gboolean start_client(gchar *cmd, GError **error)
{
    gboolean retval;
    gint argc;
    gchar **argv = NULL;


    if (!g_shell_parse_argv(cmd, &argc, &argv, error))
        return FALSE;

    retval = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, &client_pid, error);
    g_strfreev(argv);

    return retval;
}

static gboolean progress_timer(gpointer user_data)
{
    FILE *fd = user_data;
    /* it seems somehow thread safe, move to worker thread? */
    double pos = (double)ftell(fd);

    g_debug("%.2f%%", pos/total_size * 100);
    return TRUE;
}

static void free_queue(GAsyncQueue *queue)
{
    for (;;) {
        QXLCommandExt *cmd = g_async_queue_try_pop(queue);
        if (cmd == GINT_TO_POINTER(-1)) {
            continue;
        }
        if (!cmd) {
            break;
        }
        spice_replay_free_cmd(replay, cmd);
    }
    g_async_queue_unref(queue);
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    GOptionContext *context = NULL;
    gchar *client = NULL, *codecs = NULL, **file = NULL;
    gint port = 5000, compression = SPICE_IMAGE_COMPRESSION_AUTO_GLZ;
    gint streaming = SPICE_STREAM_VIDEO_FILTER;
    gboolean wait = FALSE;
    FILE *fd;

    GOptionEntry entries[] = {
        { "client", 'c', 0, G_OPTION_ARG_STRING, &client, "Client", "CMD" },
        { "compression", 'C', 0, G_OPTION_ARG_INT, &compression, "Compression (default 2)", "INT" },
        { "streaming", 'S', 0, G_OPTION_ARG_INT, &streaming, "Streaming (default 3)", "INT" },
        { "video-codecs", 'v', 0, G_OPTION_ARG_STRING, &codecs, "Video codecs", "STRING" },
        { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Server port (default 5000)", "PORT" },
        { "wait", 'w', 0, G_OPTION_ARG_NONE, &wait, "Wait for client", NULL },
        { "slow", 's', 0, G_OPTION_ARG_INT, &slow, "Slow down replay. Delays USEC microseconds before each command", "USEC" },
        { "skip", 0, 0, G_OPTION_ARG_INT, &skip, "Skip 'slow' for the first n commands", NULL },
        { "count", 0, 0, G_OPTION_ARG_NONE, &print_count, "Print the number of commands processed", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &file, "replay file", "FILE" },
        { NULL }
    };

    static const char description[] =
        "Compression values:\n"
        "\t1=off 2=auto_glz 3=auto_lz 4=quic 5=glz 6=lz 7=lz4\n"
        "\n"
        "Streaming values:\n"
        "\t1=off 2=all 3=filter";

    /* these asserts are here to check that the documentation we state above is still correct */
    G_STATIC_ASSERT(SPICE_STREAM_VIDEO_OFF == 1);
    G_STATIC_ASSERT(SPICE_STREAM_VIDEO_ALL == 2);
    G_STATIC_ASSERT(SPICE_STREAM_VIDEO_FILTER == 3);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_INVALID == 0);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_OFF == 1);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_AUTO_GLZ == 2);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_AUTO_LZ == 3);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_QUIC == 4);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_GLZ == 5);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_LZ == 6);
    G_STATIC_ASSERT(SPICE_IMAGE_COMPRESSION_LZ4 == 7);

    context = g_option_context_new("- replay spice server recording");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(1);
    }
    if (!file) {
        g_printerr("%s\n", g_option_context_get_help(context, TRUE, NULL));
        exit(1);
    }
    g_option_context_free(context);
    context = NULL;

    if (compression <= SPICE_IMAGE_COMPRESSION_INVALID
        || compression >= SPICE_IMAGE_COMPRESSION_ENUM_END) {
        g_printerr("invalid compression value\n");
        exit(1);
    }
    if (streaming < 0 || streaming == SPICE_STREAM_VIDEO_INVALID) {
        g_printerr("invalid streaming value\n");
        exit(1);
    }

    if (strncmp(file[0], "-", 1) == 0) {
        fd = stdin;
    } else {
        fd = fopen(file[0], "r");
    }
    if (fd == NULL) {
        g_printerr("error opening %s\n", file[0]);
        exit(1);
    }
    g_strfreev(file);
    file = NULL;
    if (fcntl(fileno(fd), FD_CLOEXEC) < 0) {
        perror("fcntl failed");
        exit(1);
    }
    fseek(fd, 0L, SEEK_END);
    total_size = ftell(fd);
    fseek(fd, 0L, SEEK_SET);
    if (total_size > 0)
        g_timeout_add_seconds(1, progress_timer, fd);
    replay = spice_replay_new(fd, MAX_SURFACE_NUM);
    if (replay == NULL) {
        g_printerr("Error initializing replay\n");
        exit(1);
    }

    display_queue = g_async_queue_new();
    cursor_queue = g_async_queue_new();
    core = basic_event_loop_init();
    core->channel_event = replay_channel_event;

    server = spice_server_new();
    spice_server_set_image_compression(server, compression);
    spice_server_set_streaming_video(server, streaming);

    if (codecs != NULL) {
        if (spice_server_set_video_codecs(server, codecs) != 0) {
            g_warning("could not set codecs: %s", codecs);
        }
        g_free(codecs);
    }

    spice_server_set_port(server, port);
    spice_server_set_noauth(server);

    g_print("listening on port %d (insecure)\n", port);
    spice_server_init(server, core);

    display_sin.base.sif = &display_sif.base;
    spice_server_add_interface(server, &display_sin.base);

    if (client) {
        start_client(client, &error);
        wait = TRUE;
        g_free(client);
        client = NULL;
    }

    if (!wait) {
        started = TRUE;
        fill_queue();
    }

    loop = g_main_loop_new(basic_event_loop_get_context(), FALSE);
    g_main_loop_run(loop);

    if (print_count)
        g_print("Counted %d commands\n", ncommands);

    spice_server_destroy(server);
    free_queue(display_queue);
    free_queue(cursor_queue);
    end_replay();

    /* FIXME: there should be a way to join server threads before:
     * g_main_loop_unref(loop);
     */

    return 0;
}
