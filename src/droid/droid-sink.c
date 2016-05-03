/*
 * Copyright (C) 2013 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@tieto.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/core-subscribe.h>

#include "droid-sink.h"
#include "droid-util.h"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    int32_t routing_counter;
    int32_t mute_routing_before;
    int32_t mute_routing_after;

    bool deferred_volume; /* TODO */

    pa_memblockq *memblockq;
    pa_memchunk silence;
    size_t buffer_size;
    pa_usec_t buffer_time;
    pa_usec_t write_time;
    pa_usec_t write_threshold;
    audio_devices_t prewrite_devices;
    uint32_t prewrite_silence;
    pa_hook_slot *sink_put_hook_slot;
    pa_hook_slot *sink_unlink_hook_slot;
    pa_hook_slot *sink_port_changed_hook_slot;
    pa_sink *primary_stream_sink;

    audio_devices_t primary_devices;
    audio_devices_t extra_devices;
    pa_hashmap *extra_devices_map;

    bool use_hw_volume;
    bool use_voice_volume;
    char *voice_property_key;
    char *voice_property_value;
    pa_sink_input *voice_control_sink_input;
    pa_subscription *sink_input_subscription;

    pa_hook_slot *sink_input_put_hook_slot;
    pa_hook_slot *sink_input_unlink_hook_slot;
    pa_hook_slot *sink_proplist_changed_hook_slot;
    pa_hashmap *parameters;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    pa_droid_stream *stream;
};

enum {
    SINK_MESSAGE_DO_ROUTING = PA_SINK_MESSAGE_MAX,
};

#define DEFAULT_MODULE_ID "primary"

/* sink properties */
#define PROP_DROID_PARAMETER_PREFIX "droid.parameter."
typedef struct droid_parameter_mapping {
    char *key;
    char *value;
} droid_parameter_mapping;

/* sink-input properties */
#define PROP_DROID_ROUTE "droid.device.additional-route"

/* Voice call volume control.
 * With defaults defined below, whenever sink-input with proplist key "media.role" with
 * value "phone" connects to the sink AND voice volume control is enabled, that connected
 * sink-input's absolute volume is used for HAL voice volume. */
#define DEFAULT_VOICE_CONTROL_PROPERTY_KEY      "media.role"
#define DEFAULT_VOICE_CONTROL_PROPERTY_VALUE    "phone"

static void parameter_free(droid_parameter_mapping *m);
static void userdata_free(struct userdata *u);
static void set_voice_volume(struct userdata *u, pa_sink_input *i);

static void set_primary_devices(struct userdata *u, audio_devices_t devices) {
    pa_assert(u);
    pa_assert(devices);

    u->primary_devices = devices;
}

static bool add_extra_devices(struct userdata *u, audio_devices_t devices) {
    void *value;
    uint32_t count;
    bool need_update = false;

    pa_assert(u);
    pa_assert(u->extra_devices_map);
    pa_assert(devices);

    if ((value = pa_hashmap_get(u->extra_devices_map, PA_UINT_TO_PTR(devices)))) {
        count = PA_PTR_TO_UINT(value);
        count++;
        pa_hashmap_remove(u->extra_devices_map, PA_UINT_TO_PTR(devices));
        pa_hashmap_put(u->extra_devices_map, PA_UINT_TO_PTR(devices), PA_UINT_TO_PTR(count));

        /* added extra device already exists in hashmap, so no need to update route. */
        need_update = false;
    } else {
        pa_hashmap_put(u->extra_devices_map, PA_UINT_TO_PTR(devices), PA_UINT_TO_PTR(1));
        u->extra_devices |= devices;
        need_update = true;
    }

    return need_update;
}

static bool remove_extra_devices(struct userdata *u, audio_devices_t devices) {
    void *value;
    uint32_t count;
    bool need_update = false;

    pa_assert(u);
    pa_assert(u->extra_devices_map);
    pa_assert(devices);

    if ((value = pa_hashmap_get(u->extra_devices_map, PA_UINT_TO_PTR(devices)))) {
        pa_hashmap_remove(u->extra_devices_map, PA_UINT_TO_PTR(devices));
        count = PA_PTR_TO_UINT(value);
        count--;
        if (count == 0) {
            u->extra_devices &= ~devices;
            need_update = true;
        } else {
            /* added extra devices still exists in hashmap, so no need to update route. */
            pa_hashmap_put(u->extra_devices_map, PA_UINT_TO_PTR(devices), PA_UINT_TO_PTR(count));
            need_update = false;
        }
    }

    return need_update;
}

static void clear_extra_devices(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->extra_devices_map);

    pa_hashmap_remove_all(u->extra_devices_map);
    u->extra_devices = 0;
}

/* Called from main context during voice calls, and from IO context during media operation. */
static void do_routing(struct userdata *u) {
    audio_devices_t routing;

    pa_assert(u);
    pa_assert(u->stream);

    if (u->use_voice_volume && u->extra_devices)
        clear_extra_devices(u);

    routing = u->primary_devices | u->extra_devices;

    pa_droid_stream_set_output_route(u->stream, routing);
}

static bool parse_device_list(const char *str, audio_devices_t *dst) {
    pa_assert(str);
    pa_assert(dst);

    char *dev;
    const char *state = NULL;

    *dst = 0;

    while ((dev = pa_split(str, "|", &state))) {
        audio_devices_t d;

        if (!pa_string_convert_output_device_str_to_num(dev, &d)) {
            pa_log_warn("Unknown device %s", dev);
            pa_xfree(dev);
            return false;
        }

        *dst |= d;

        pa_xfree(dev);
    }

    return true;
}

static int thread_write_silence(struct userdata *u) {
    const void *p;
    ssize_t wrote;

    /* Drop our rendered audio and write silence to HAL. */
    pa_memblockq_drop(u->memblockq, u->buffer_size);
    u->write_time = pa_rtclock_now();

    /* We should be able to write everything in one go as long as memblock size
     * is multiples of buffer_size. Even if we don't write whole buffer size
     * here it's okay, as long as mute time isn't configured too strictly. */

    p = pa_memblock_acquire(u->silence.memblock);
    wrote = u->stream->out->write(u->stream->out, (const uint8_t*) p + u->silence.index, u->silence.length);
    pa_memblock_release(u->silence.memblock);

    u->write_time = pa_rtclock_now() - u->write_time;

    if (wrote < 0)
        return -1;

    return 0;
}

static int thread_write(struct userdata *u) {
    pa_memchunk c;
    const void *p;
    ssize_t wrote;

    pa_memblockq_peek_fixed_size(u->memblockq, u->buffer_size, &c);

    /* We should be able to write everything in one go as long as memblock size
     * is multiples of buffer_size. */

    u->write_time = pa_rtclock_now();

    for (;;) {
        p = pa_memblock_acquire(c.memblock);
        wrote = u->stream->out->write(u->stream->out, (const uint8_t*) p + c.index, c.length);
        pa_memblock_release(c.memblock);

        if (wrote < 0) {
            pa_memblockq_drop(u->memblockq, c.length);
            pa_memblock_unref(c.memblock);
            u->write_time = 0;
            pa_log("failed to write stream (%d)", wrote);
            return -1;
        }

        if (wrote < (ssize_t) c.length) {
            c.index += wrote;
            c.length -= wrote;
            continue;
        }

        pa_memblockq_drop(u->memblockq, c.length);
        pa_memblock_unref(c.memblock);

        break;
    }

    u->write_time = pa_rtclock_now() - u->write_time;

    return 0;
}
static void thread_render(struct userdata *u) {
    size_t length;
    size_t missing;

    length = pa_memblockq_get_length(u->memblockq);
    missing = u->buffer_size - length;

    if (missing > 0) {
        pa_memchunk c;
        pa_sink_render_full(u->sink, missing, &c);
        pa_memblockq_push_align(u->memblockq, &c);
        pa_memblock_unref(c.memblock);
    }
}

static void process_rewind(struct userdata *u) {
    size_t rewind_nbytes;
    size_t max_rewind_nbytes;
    size_t queue_length;

    pa_assert(u);

    if (u->sink->thread_info.rewind_nbytes == 0) {
        pa_sink_process_rewind(u->sink, 0);
        return;
    }

    rewind_nbytes = u->sink->thread_info.rewind_nbytes;
    u->sink->thread_info.rewind_nbytes = 0;

    pa_assert(rewind_nbytes > 0);
    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    queue_length = pa_memblockq_get_length(u->memblockq);
    if (queue_length <= u->buffer_size)
        goto do_nothing;
    max_rewind_nbytes = queue_length - u->buffer_size;
    if (max_rewind_nbytes == 0)
        goto do_nothing;

    if (rewind_nbytes > max_rewind_nbytes)
        rewind_nbytes = max_rewind_nbytes;

    pa_memblockq_drop(u->memblockq, rewind_nbytes);

    pa_sink_process_rewind(u->sink, rewind_nbytes);

    pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
    return;

do_nothing:
    pa_log_debug("Rewound 0 bytes.");
    pa_sink_process_rewind(u->sink, 0);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up.");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {

            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                process_rewind(u);

            if (pa_rtpoll_timer_elapsed(u->rtpoll)) {
                pa_usec_t sleept = 0;

                thread_render(u);

                if (u->routing_counter == u->mute_routing_after) {
                    do_routing(u);
                    u->routing_counter--;
                } else if (u->routing_counter > -1) {
                    thread_write_silence(u);
                    u->routing_counter--;
                } else
                    thread_write(u);

                if (u->write_time > u->write_threshold)
                    sleept = u->buffer_time;

                pa_rtpoll_set_timer_relative(u->rtpoll, sleept);
            }
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Sleep */
#if (PULSEAUDIO_VERSION == 5)
        if ((ret = pa_rtpoll_run(u->rtpoll, true)) < 0)
#elif (PULSEAUDIO_VERSION >= 6)
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
#endif
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down.");
}

/* Called from IO context */
static int suspend(struct userdata *u) {
    int ret;
    size_t length;

    pa_assert(u);
    pa_assert(u->sink);
    pa_assert(u->stream->out);

    ret = pa_droid_stream_suspend(u->stream, true);

    if (ret == 0) {
        pa_sink_set_max_request_within_thread(u->sink, 0);
        pa_log_info("Device suspended.");
    } else
        pa_log("Couldn't set standby, err %d", ret);

    /* Clear memblockq */
    if ((length = pa_memblockq_get_length(u->memblockq)) > 0)
        pa_memblockq_drop(u->memblockq, length);

    return ret;
}

static int unsuspend(struct userdata *u) {
    uint32_t i;

    pa_assert(u);
    pa_assert(u->sink);

    /* HAL resumes automagically when writing to standby stream, but let's set max request */
    pa_sink_set_max_request_within_thread(u->sink, u->buffer_size);

    pa_log_info("Resuming...");

    if (u->prewrite_silence &&
        (u->primary_devices | u->extra_devices) & u->prewrite_devices &&
        pa_droid_output_stream_any_active(u->stream) == 0) {
        for (i = 0; i < u->prewrite_silence; i++)
            thread_write_silence(u);
    }

    pa_droid_stream_suspend(u->stream, false);

    return 0;
}

/* Called from IO context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case SINK_MESSAGE_DO_ROUTING: {
            /* When mute_routing_before & mute_routing_after are 0, routing change is done
             * immediately when next round in thread_func. Otherwise write silence until
             * counter equals mute_routing_after, execute routing, and write silence until
             * routing_counter is 0. */
            u->routing_counter = u->mute_routing_before + u->mute_routing_after;
            return 0;
        }

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            /* HAL reports milliseconds */
            if (u->stream->out)
                r = u->stream->out->get_latency(u->stream->out) * PA_USEC_PER_MSEC;

            *((pa_usec_t*) data) = r;

            return 0;
        }

        case PA_SINK_MESSAGE_SET_STATE: {
            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SINK_SUSPENDED: {
                    int r;

                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    if ((r = suspend(u)) < 0)
                        return r;

                    break;
                }

                case PA_SINK_IDLE:
                    /* Fall through */
                case PA_SINK_RUNNING: {
                    int r;

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        if ((r = unsuspend(u)) < 0)
                            return r;
                    }

                    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
                    break;
                }

                case PA_SINK_UNLINKED: {
                    /* Suspending since some implementations do not want to free running stream. */
                    suspend(u);
                    break;
                }

                /* not needed */
                case PA_SINK_INIT:
                case PA_SINK_INVALID_STATE:
                    ;
            }
            break;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    struct userdata *u = s->userdata;
    pa_droid_port_data *data;

    pa_assert(u);
    pa_assert(p);

    data = PA_DEVICE_PORT_DATA(p);

    if (!data->device) {
        /* If there is no device defined, just return 0 to say everything is ok.
         * Then next port change can be whatever sink port, even the one enabled
         * before parking. */
        pa_log_debug("Sink set port to parking");
        return 0;
    }

    pa_log_debug("Sink set port %u", data->device);

    set_primary_devices(u, data->device);
    /* If we are in voice call, sink is usually in suspended state and routing change can be applied immediately.
     * When in media use cases, do the routing change in IO thread if we are currently in RUNNING or IDLE state. */
    if (u->use_voice_volume || !PA_SINK_IS_OPENED(pa_sink_get_state(u->sink)))
        do_routing(u);
    else {
        pa_asyncmsgq_post(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_DO_ROUTING, NULL, 0, NULL, NULL);
    }

    return 0;
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&r, &s->real_volume, s->base_volume);

    if (r.channels == 1) {
        float val = pa_sw_volume_to_linear(r.values[0]);
        pa_log_debug("Set hw volume %f", val);
        pa_droid_hw_module_lock(u->hw_module);
        if (u->stream->out->set_volume(u->stream->out, val, val) < 0)
            pa_log_warn("Failed to set hw volume.");
        pa_droid_hw_module_unlock(u->hw_module);
    } else if (r.channels == 2) {
        float val[2];
        for (unsigned i = 0; i < 2; i++)
            val[i] = pa_sw_volume_to_linear(r.values[i]);
        pa_log_debug("Set hw volume %f : %f", val[0], val[1]);
        pa_droid_hw_module_lock(u->hw_module);
        if (u->stream->out->set_volume(u->stream->out, val[0], val[1]) < 0)
            pa_log_warn("Failed to set hw volume.");
        pa_droid_hw_module_unlock(u->hw_module);
    }
}

/* Called from main thread */
static void set_voice_volume(struct userdata *u, pa_sink_input *i) {
    pa_cvolume vol;
    float val;

    pa_assert_ctl_context();
    pa_assert(u);
    pa_assert(i);

    pa_sink_input_get_volume(i, &vol, true);

    val = pa_sw_volume_to_linear(pa_cvolume_avg(&vol));
    pa_log_debug("Set voice volume %f", val);

    pa_droid_hw_module_lock(u->hw_module);
    if (u->hw_module->device->set_voice_volume(u->hw_module->device, val) < 0)
        pa_log_warn("Failed to set voice volume.");
    pa_droid_hw_module_unlock(u->hw_module);
}

static void update_volumes(struct userdata *u) {
    int ret = -1;

    /* set_volume returns 0 if hw volume control is implemented, < 0 otherwise. */
    pa_droid_hw_module_lock(u->hw_module);
    if (u->stream->out->set_volume) {
        pa_log_debug("Probe hw volume support for %s", u->sink->name);
        ret = u->stream->out->set_volume(u->stream->out, 1.0f, 1.0f);
    }
    pa_droid_hw_module_unlock(u->hw_module);

    u->use_hw_volume = (ret == 0);

    if (pa_droid_stream_is_primary(u->stream)) {
        /* Apply callbacks */
        pa_droid_sink_set_voice_control(u->sink, false);
    }
}

static void set_sink_name(pa_modargs *ma, pa_sink_new_data *data, const char *module_id) {
    const char *tmp;

    pa_assert(ma);
    pa_assert(data);

    if ((tmp = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_sink_new_data_set_name(data, tmp);
        data->namereg_fail = true;
        pa_proplist_sets(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid sink");
    } else {
        char *tt;
        pa_assert(module_id);
        tt = pa_sprintf_malloc("sink.%s", module_id);
        pa_sink_new_data_set_name(data, tt);
        pa_xfree(tt);
        data->namereg_fail = false;
        pa_proplist_setf(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid sink %s", module_id);
    }
}

/* Called from main thread */
static pa_sink_input *find_volume_control_sink_input(struct userdata *u) {
    const char *val;
    uint32_t idx;
    pa_sink_input *i;

    pa_assert_ctl_context();
    pa_assert(u);
    pa_assert(u->sink);

    PA_IDXSET_FOREACH(i, u->sink->inputs, idx) {
        if ((val = pa_proplist_gets(i->proplist, u->voice_property_key))) {
            if (pa_streq(val, u->voice_property_value)) {
                return i;
            }
        }
    }

    return NULL;
}

/* Called from main thread */
static void sink_input_subscription_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, struct userdata *u) {
    pa_sink_input *i;

    pa_assert_ctl_context();

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_NEW) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_CHANGE) &&
        t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_REMOVE))
        return;

    if (!(i = pa_idxset_get_by_index(c->sink_inputs, idx)))
        return;

    if (t == (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_NEW)) {
        if (!u->voice_control_sink_input && (i = find_volume_control_sink_input(u))) {
            u->voice_control_sink_input = i;
            set_voice_volume(u, i);
        }
    }
    else if (t == (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_CHANGE)) {
        if (u->voice_control_sink_input == i)
            set_voice_volume(u, i);
    }
    else if (t == (PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_REMOVE)) {
        if (u->voice_control_sink_input == i)
            u->voice_control_sink_input = NULL;
    }
}

/* Called from main thread */
void pa_droid_sink_set_voice_control(pa_sink* sink, bool enable) {
    pa_sink_input *i;
    struct userdata *u;

    pa_assert_ctl_context();
    pa_assert(sink);

    u = sink->userdata;
    pa_assert(u);
    pa_assert(u->sink == sink);

    if (!pa_droid_stream_is_primary(u->stream)) {
        pa_log_debug("skipping voice volume control with non-primary stream");
        return;
    }

    if (u->use_voice_volume == enable)
        return;

    u->use_voice_volume = enable;

    if (u->use_voice_volume) {
        pa_log_debug("Using voice volume control for %s", u->sink->name);
        pa_sink_set_set_volume_callback(u->sink, NULL);

        /* Susbcription tracking voice call volume control sink-input is set up when
         * voice volume control is enabled. In case volume control sink-input has already
         * connected to the sink, check for the sink-input here as well. */

        if (!u->sink_input_subscription)
            u->sink_input_subscription = pa_subscription_new(u->core,
                                                             PA_SUBSCRIPTION_MASK_SINK_INPUT,
                                                             (pa_subscription_cb_t) sink_input_subscription_cb,
                                                             u);

        if ((i = find_volume_control_sink_input(u))) {
            u->voice_control_sink_input = i;
            set_voice_volume(u, i);
        }

    } else {
        if (u->sink_input_subscription) {
            pa_subscription_free(u->sink_input_subscription);
            u->sink_input_subscription = NULL;
            u->voice_control_sink_input = NULL;
        }

        if (u->use_hw_volume) {
            pa_log_debug("Using hardware volume control for %s", u->sink->name);
            pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
        } else {
            pa_log_debug("Using software volume control for %s", u->sink->name);
            pa_sink_set_set_volume_callback(u->sink, NULL);
        }
    }
}

/* When sink-input with proper proplist variable appears, do extra routing configuration
 * for the lifetime of that sink-input. */
static pa_hook_result_t sink_input_put_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    const char *media_str;
    audio_devices_t devices;

    /* Dynamic routing changes do not apply during active voice call. */
    if (u->use_voice_volume)
        return PA_HOOK_OK;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        /* Do not change routing for gstreamer pulsesink probe. Workaround for unnecessary routing changes when gst-plugin
         * pulsesink connects to our sink. Not the best fix or the best place for a fix, but let's have this here
         * for now anyway. */
        if ((media_str = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_NAME)) && pa_streq(media_str, "pulsesink probe"))
            return PA_HOOK_OK;

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("Add extra route %s (%u).", dev_str, devices);

            /* if this device was not routed to previously post routing change */
            if (add_extra_devices(u, devices))
                pa_asyncmsgq_post(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_DO_ROUTING, NULL, 0, NULL, NULL);
        }
    }

    return PA_HOOK_OK;
}

/* Remove extra routing when sink-inputs disappear. */
static pa_hook_result_t sink_input_unlink_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    const char *media_str;
    audio_devices_t devices;

    /* Dynamic routing changes do not apply during active voice call. */
    if (u->use_voice_volume)
        return PA_HOOK_OK;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        /* Do not change routing for gstreamer pulsesink probe. Workaround for unnecessary routing changes when gst-plugin
         * pulsesink connects to our sink. Not the best fix or the best place for a fix, but let's have this here
         * for now anyway. */
        if ((media_str = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_NAME)) && pa_streq(media_str, "pulsesink probe"))
            return PA_HOOK_OK;

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("Remove extra route %s (%u).", dev_str, devices);

            /* if this device no longer exists in extra devices map post routing change */
            if (remove_extra_devices(u, devices))
                pa_asyncmsgq_post(u->sink->asyncmsgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_DO_ROUTING, NULL, 0, NULL, NULL);
        }
    }

    return PA_HOOK_OK;
}

/* Watch for properties starting with droid.parameter. and translate them directly to
 * HAL set_parameters() calls. */
static pa_hook_result_t sink_proplist_changed_hook_cb(pa_core *c, pa_sink *sink, struct userdata *u) {
    bool changed = false;
    const char *pkey;
    const char *key;
    const char *value;
    char *tmp;
    void *state = NULL;
    droid_parameter_mapping *parameter = NULL;

    pa_assert(sink);
    pa_assert(u);

    if (u->sink != sink)
        return PA_HOOK_OK;

    while ((key = pa_proplist_iterate(sink->proplist, &state))) {
        if (!pa_startswith(key, PROP_DROID_PARAMETER_PREFIX))
            continue;

        pkey = key + strlen(PROP_DROID_PARAMETER_PREFIX);
        if (pkey[0] == '\0')
            continue;

        changed = false;

        if (!(parameter = pa_hashmap_get(u->parameters, pkey))) {
            parameter = pa_xnew0(droid_parameter_mapping, 1);
            parameter->key = pa_xstrdup(pkey);
            parameter->value = pa_xstrdup(pa_proplist_gets(sink->proplist, key));
            pa_hashmap_put(u->parameters, parameter->key, parameter);
            changed = true;
        } else {
            value = pa_proplist_gets(sink->proplist, key);
            if (!pa_streq(parameter->value, value)) {
                pa_xfree(parameter->value);
                parameter->value = pa_xstrdup(value);
                changed = true;
            }
        }

        if (changed) {
            pa_assert(parameter);
            tmp = pa_sprintf_malloc("%s=%s;", parameter->key, parameter->value);
            pa_log_debug("set_parameters(): %s", tmp);
            pa_droid_stream_set_parameters(u->stream, tmp);
            pa_xfree(tmp);
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_port_changed_hook_cb(pa_core *c, pa_sink *sink, struct userdata *u) {
    pa_device_port *port;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

    if (sink != u->primary_stream_sink)
        return PA_HOOK_OK;

    port = sink->active_port;
    pa_log_info("Set slave sink port to %s", port->name);
    pa_sink_set_port(u->sink, port->name, false);

    return PA_HOOK_OK;
}

static void unset_primary_stream_sink(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->primary_stream_sink);
    pa_assert(u->sink_port_changed_hook_slot);

    pa_hook_slot_free(u->sink_port_changed_hook_slot);
    u->sink_port_changed_hook_slot = NULL;
    u->primary_stream_sink = NULL;
}

static pa_hook_result_t sink_unlink_hook_cb(pa_core *c, pa_sink *sink, struct userdata *u) {
    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

    if (sink != u->primary_stream_sink)
        return PA_HOOK_OK;

    pa_log_info("Primary stream sink disappeared.");
    unset_primary_stream_sink(u);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_put_hook_cb(pa_core *c, pa_sink *sink, struct userdata *u) {
    struct userdata *sink_u;

    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

    if (!pa_sink_is_droid_sink(sink))
        return PA_HOOK_OK;

    sink_u = sink->userdata;

    if (!pa_droid_stream_is_primary(sink_u->stream))
        return PA_HOOK_OK;

    u->primary_stream_sink = sink;

    pa_assert(!u->sink_port_changed_hook_slot);
    u->sink_port_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_PORT_CHANGED], PA_HOOK_NORMAL,
            (pa_hook_cb_t) sink_port_changed_hook_cb, u);

    pa_log_info("Primary stream sink setup for slave.");

    sink_port_changed_hook_cb(c, sink, u);

    return PA_HOOK_OK;
}

static void setup_track_primary(struct userdata *u) {
    pa_sink *sink;
    struct userdata *sink_u;
    uint32_t idx;

    pa_assert(u);

    u->sink_put_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_NORMAL,
            (pa_hook_cb_t) sink_put_hook_cb, u);
    u->sink_unlink_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_NORMAL,
            (pa_hook_cb_t) sink_unlink_hook_cb, u);

    PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
        if (pa_sink_is_droid_sink(sink)) {
            sink_u = sink->userdata;
            if (pa_droid_stream_is_primary(sink_u->stream)) {
                sink_put_hook_cb(u->core, sink, u);
                break;
            }
        }
    }
}

static bool parse_prewrite_on_resume(struct userdata *u, const char *prewrite_resume, const char *name) {
    const char *state = NULL;
    char *entry = NULL;
    char *devices, *stream, *value;
    uint32_t devices_len, devices_index, value_index, entry_len;
    uint32_t b;

    pa_assert(u);
    pa_assert(prewrite_resume);
    pa_assert(name);

    /* Argument is string of for example "deep_buffer=AUDIO_DEVICE_OUT_SPEAKER:1,primary=FOO:5" */

    while ((entry = pa_split(prewrite_resume, ",", &state))) {

        entry_len = strlen(entry);
        devices_index = strcspn(entry, "=");

        if (devices_index == 0 || devices_index >= entry_len - 1)
            goto error;

        entry[devices_index] = '\0';
        devices = entry + devices_index + 1;
        stream = entry;

        devices_len = strlen(devices);
        value_index = strcspn(devices, ":");

        if (value_index == 0 || value_index >= devices_len - 1)
            goto error;

        devices[value_index] = '\0';
        value = devices + value_index + 1;

        if (!parse_device_list(devices, &u->prewrite_devices)) {
            u->prewrite_devices = 0;
            goto error;
        }

        if (strlen(value) == 0 || pa_atou(value, &b) < 0)
            goto error;

        if (pa_streq(stream, name)) {
            pa_log_info("Using requested prewrite size for %s: %u (%u * %u).",
                        name, u->buffer_size * b, b, u->buffer_size);
            u->prewrite_silence = b;
            pa_xfree(entry);
            return true;
        }

        pa_xfree(entry);
    }

return true;

error:
    pa_xfree(entry);
    return false;
}

pa_sink *pa_droid_sink_new(pa_module *m,
                             pa_modargs *ma,
                             const char *driver,
                             pa_droid_card_data *card_data,
                             audio_output_flags_t flags,
                             pa_droid_mapping *am,
                             pa_card *card) {

    struct userdata *u = NULL;
    bool deferred_volume = false;
    char *thread_name = NULL;
    pa_sink_new_data data;
    const char *module_id = NULL;
    const char *tmp;
    char *list = NULL;
    uint32_t alternate_sample_rate;
    const char *format;
    audio_devices_t dev_out;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    bool namereg_fail = false;
    pa_usec_t latency;
    pa_droid_config_audio *config = NULL; /* Only used when sink is created without card */
    int32_t mute_routing_before = 0;
    int32_t mute_routing_after = 0;
    uint32_t sink_buffer = 0;
    const char *prewrite_resume = NULL;
    int ret;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    deferred_volume = m->core->deferred_volume;
    if (pa_modargs_get_value_boolean(ma, "deferred_volume", &deferred_volume) < 0) {
        pa_log("Failed to parse deferred_volume argument.");
        goto fail;
    }

    if (card && am)
        module_id = am->output->module->name;
    else
        module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    sample_spec = m->core->default_sample_spec;
    channel_map = m->core->default_channel_map;

    /* First parse both sample spec and channel map, then see if sink_* override some
     * of the values. */
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &sample_spec, &channel_map, PA_CHANNEL_MAP_AIFF) < 0) {
        pa_log("Failed to parse sink sample specification and channel map.");
        goto fail;
    }

    if (pa_modargs_get_value(ma, "sink_channel_map", NULL)) {
        if (pa_modargs_get_channel_map(ma, "sink_channel_map", &channel_map) < 0) {
            pa_log("Failed to parse sink channel map.");
            goto fail;
        }

        sample_spec.channels = channel_map.channels;
    }

    if ((format = pa_modargs_get_value(ma, "sink_format", NULL))) {
        if ((sample_spec.format = pa_parse_sample_format(format)) < 0) {
            pa_log("Failed to parse sink format.");
            goto fail;
        }
    }

    if (pa_modargs_get_value_u32(ma, "sink_rate", &sample_spec.rate) < 0) {
        pa_log("Failed to parse sink samplerate");
        goto fail;
    }

    if (!pa_sample_spec_valid(&sample_spec)) {
        pa_log("Sample spec is not valid.");
        goto fail;
    }

    alternate_sample_rate = m->core->alternate_sample_rate;
    if (pa_modargs_get_alternate_sample_rate(ma, &alternate_sample_rate) < 0) {
        pa_log("Failed to parse alternate sample rate.");
        goto fail;
    }

    if ((pa_modargs_get_value_s32(ma, "mute_routing_before", &mute_routing_before) < 0) || mute_routing_before < 0) {
        pa_log("Failed to parse mute_routing_before. Needs to be integer >= 0.");
        goto fail;
    }

    if ((pa_modargs_get_value_s32(ma, "mute_routing_after", &mute_routing_after) < 0) || mute_routing_after < 0) {
        pa_log("Failed to parse mute_routing_after. Needs to be integer >= 0.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "sink_buffer", &sink_buffer) < 0) {
        pa_log("Failed to parse sink_buffer. Needs to be integer >= 0.");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->card = card;
    u->deferred_volume = deferred_volume;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    u->parameters = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                        NULL, (pa_free_cb_t) parameter_free);
    u->voice_property_key   = pa_xstrdup(pa_modargs_get_value(ma, "voice_property_key", DEFAULT_VOICE_CONTROL_PROPERTY_KEY));
    u->voice_property_value = pa_xstrdup(pa_modargs_get_value(ma, "voice_property_value", DEFAULT_VOICE_CONTROL_PROPERTY_VALUE));
    u->extra_devices_map = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    if (card_data) {
        u->card_data = card_data;
        pa_assert(card);
        pa_assert_se((u->hw_module = pa_droid_hw_module_get(u->core, NULL, card_data->module_id)));
    } else {
        /* Sink wasn't created from inside card module, so we'll need to open
         * hw module ourself.
         *
         * First let's find out if hw module has already been opened, or if we need to
         * do it ourself.
         */
        if (!(u->hw_module = pa_droid_hw_module_get(u->core, NULL, module_id))) {

            /* No hw module object in shared object db, let's open the module now. */

            if (!(config = pa_droid_config_load(ma)))
                goto fail;

            /* Ownership of config transfers to hw_module if opening of hw module succeeds. */
            if (!(u->hw_module = pa_droid_hw_module_get(u->core, config, module_id)))
                goto fail;
        }
    }

    /* Default routing */
    dev_out = u->hw_module->config->global_config.default_output_device;

    if ((tmp = pa_modargs_get_value(ma, "output_devices", NULL))) {
        audio_devices_t tmp_dev;

        if (parse_device_list(tmp, &tmp_dev) && tmp_dev)
            dev_out = tmp_dev;

        pa_log_debug("Set initial devices %s", tmp);
    }

    if (am)
        flags = am->output->flags;

    u->stream = pa_droid_open_output_stream(u->hw_module, &sample_spec, &channel_map, flags, dev_out);

    if (!u->stream) {
        pa_log("Failed to open output stream.");
        goto fail;
    }

    u->buffer_size = u->stream->out->common.get_buffer_size(&u->stream->out->common);
    if (sink_buffer) {
        if (sink_buffer < u->buffer_size)
            pa_log_warn("Requested buffer size %u less than HAL reported buffer size (%u).", sink_buffer, u->buffer_size);
        else if (sink_buffer % u->buffer_size) {
            uint32_t trunc = (sink_buffer / u->buffer_size) * u->buffer_size;
            pa_log_warn("Requested buffer size %u not multiple of HAL buffer size (%u). Using buffer size %u", sink_buffer, u->buffer_size, trunc);
            u->buffer_size = trunc;
        } else {
            pa_log_info("Using requested buffer size %u.", sink_buffer);
            u->buffer_size = sink_buffer;
        }
    }

    if ((prewrite_resume = pa_modargs_get_value(ma, "prewrite_on_resume", NULL))) {
        if (!parse_prewrite_on_resume(u, prewrite_resume, am ? am->output->name : module_id)) {
            pa_log("Failed to parse prewrite_on_resume (%s)", prewrite_resume);
            goto fail;
        }
    }

    u->buffer_time = pa_bytes_to_usec(u->buffer_size, &u->stream->sample_spec);

    u->write_threshold = u->buffer_time - u->buffer_time / 6;
    u->mute_routing_before = mute_routing_before / u->buffer_size;
    u->mute_routing_after = mute_routing_after / u->buffer_size;
    if (u->mute_routing_before == 0 && mute_routing_before)
        u->mute_routing_before = 1;
    if (u->mute_routing_after == 0 && mute_routing_after)
        u->mute_routing_after = 1;
    if (u->mute_routing_before || u->mute_routing_after)
        pa_log_debug("Mute playback when routing is changing, %u before and %u after.",
                     u->mute_routing_before * u->buffer_size,
                     u->mute_routing_after * u->buffer_size);
    pa_silence_memchunk_get(&u->core->silence_cache, u->core->mempool, &u->silence, &u->stream->sample_spec, u->buffer_size);
    u->memblockq = pa_memblockq_new("droid-sink", 0, u->buffer_size, u->buffer_size, &u->stream->sample_spec, 1, 0, 0, &u->silence);

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;

    if (am)
        set_sink_name(ma, &data, am->output->name);
    else
        set_sink_name(ma, &data, module_id);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, PROP_DROID_API_STRING);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_sink_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    pa_sink_new_data_set_sample_spec(&data, &u->stream->sample_spec);
    pa_sink_new_data_set_channel_map(&data, &u->stream->channel_map);
    pa_sink_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    /*
    if (!(list = pa_list_string_output_device(dev_out))) {
        pa_log("Couldn't format device list string.");
        goto fail;
    }
    pa_proplist_sets(data.proplist, PROP_DROID_DEVICES, list);
    pa_xfree(list);
    */

    if (flags) {
        if (!(list = pa_list_string_flags(flags))) {
            pa_log("Couldn't format flag list string.");
            goto fail;
        }
    } else
        list = NULL;

    pa_proplist_sets(data.proplist, PROP_DROID_FLAGS, list ? list : "");
    pa_xfree(list);

    if (am)
        pa_droid_add_ports(data.ports, am, card);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE | PA_SINK_LATENCY | PA_SINK_FLAT_VOLUME);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->userdata = u;

    u->sink->parent.process_msg = sink_process_msg;

    u->sink->set_port = sink_set_port_cb;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* Rewind internal memblockq */
    pa_sink_set_max_rewind(u->sink, 0);

    if (am)
        thread_name = pa_sprintf_malloc("droid-sink-%s", am->output->name);
    else
        thread_name = pa_sprintf_malloc("droid-sink-%s", module_id);
    if (!(u->thread = pa_thread_new(thread_name, thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    /* HAL latencies are in milliseconds. */
    latency = u->stream->out->get_latency(u->stream->out) * PA_USEC_PER_MSEC;
    pa_sink_set_fixed_latency(u->sink, latency);
    pa_log_debug("Set fixed latency %llu usec", latency);
    pa_sink_set_max_request(u->sink, u->buffer_size);

    if (u->sink->active_port)
        sink_set_port_cb(u->sink, u->sink->active_port);

    /* Hooks to track appearance and disappearance of sink-inputs. */
    /* Hook a little bit earlier and later than module-role-ducking. */
    u->sink_input_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE+10,
            (pa_hook_cb_t) sink_input_put_hook_cb, u);
    u->sink_input_unlink_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_EARLY-10,
            (pa_hook_cb_t) sink_input_unlink_hook_cb, u);
    u->sink_proplist_changed_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], PA_HOOK_EARLY,
            (pa_hook_cb_t) sink_proplist_changed_hook_cb, u);

    update_volumes(u);

    if (!pa_droid_stream_is_primary(u->stream))
        setup_track_primary(u);

    pa_droid_stream_suspend(u->stream, false);
    pa_sink_put(u->sink);

    return u->sink;

fail:
    pa_xfree(thread_name);

    if (config)
        pa_xfree(config);

    if (u)
        userdata_free(u);

    return NULL;
}

void pa_droid_sink_free(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}

static void parameter_free(droid_parameter_mapping *m) {
    pa_assert(m);

    pa_xfree(m->key);
    pa_xfree(m->value);
    pa_xfree(m);
}

static void userdata_free(struct userdata *u) {

    if (u->primary_stream_sink)
        unset_primary_stream_sink(u);

    if (u->sink_put_hook_slot)
        pa_hook_slot_free(u->sink_put_hook_slot);

    if (u->sink_unlink_hook_slot)
        pa_hook_slot_free(u->sink_unlink_hook_slot);

    if (u->sink_port_changed_hook_slot)
        pa_hook_slot_free(u->sink_port_changed_hook_slot);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink_input_subscription)
        pa_subscription_free(u->sink_input_subscription);

    if (u->sink_input_put_hook_slot)
        pa_hook_slot_free(u->sink_input_put_hook_slot);

    if (u->sink_input_unlink_hook_slot)
        pa_hook_slot_free(u->sink_input_unlink_hook_slot);

    if (u->sink_proplist_changed_hook_slot)
        pa_hook_slot_free(u->sink_proplist_changed_hook_slot);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->parameters)
        pa_hashmap_free(u->parameters);

    if (u->stream)
        pa_droid_stream_unref(u->stream);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);

    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    if (u->voice_property_key)
        pa_xfree(u->voice_property_key);
    if (u->voice_property_value)
        pa_xfree(u->voice_property_value);

    if (u->extra_devices_map)
        pa_hashmap_free(u->extra_devices_map);

    pa_xfree(u);
}
