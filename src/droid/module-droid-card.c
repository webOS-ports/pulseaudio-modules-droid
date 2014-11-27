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
#include <pulsecore/source.h>
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
#include <pulsecore/card.h>
#include <pulsecore/device-port.h>
#include <pulsecore/idxset.h>

#include <hardware/audio.h>
#include <system/audio.h>

//#include <droid/hardware/audio_policy.h>
//#include <droid/system/audio_policy.h>

#include "droid-util.h"
#include "droid-sink.h"
#include "droid-source.h"

#ifdef HAVE_UDEV
#include "droid-extcon.h"
#endif

#include "module-droid-card-symdef.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Droid card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
        "card_name=<name for the card> "
        "sink_name=<name for the sink> "
        "source_name=<name for the source> "
        "namereg_fail=<when false attempt to synthesise new names if they are already taken> "
        "rate=<sample rate> "
        "output_flags=<flags for sink> "
        "module_id=<which droid hw module to load, default primary> "
        "voice_source_routing=<route source ports during voice call, default false> "
        "deferred_volume=<synchronize software and hardware volume changes to avoid momentary jumps?> "
        "config=<location for droid audio configuration> "
        "voice_property_key=<proplist key searched for sink-input that should control voice call volume> "
        "voice_property_value=<proplist value for the key for voice control sink-input>"
);

static const char* const valid_modargs[] = {
    "card_name",
    "sink_name",
    "source_name",
    "namereg_fail",
    "format",
    "rate",
    "output_flags",
    "module_id",
    "voice_source_routing",
    "sink_buffer",
    "source_buffer",
    "deferred_volume",
    "mute_routing_before",
    "mute_routing_after",
    "config",
    "voice_property_key",
    "voice_property_value",
    NULL,
};

#define DEFAULT_MODULE_ID "primary"
#define DEFAULT_AUDIO_POLICY_CONF "/system/etc/audio_policy.conf"
#define VOICE_CALL_PROFILE_NAME     "voicecall"
#define VOICE_CALL_PROFILE_DESC     "Call mode"
#define VOICE_RECORD_PROFILE_NAME   "voicecall-record"
#define VOICE_RECORD_PROFILE_DESC   "Call mode record"
#define RINGTONE_PROFILE_NAME       "ringtone"
#define RINGTONE_PROFILE_DESC       "Ringtone mode"
#define COMMUNICATION_PROFILE_NAME  "communication"
#define COMMUNICATION_PROFILE_DESC  "Communication mode"

struct userdata;

typedef bool (*virtual_profile_event_cb)(struct userdata *u, pa_droid_profile *p, bool enabling);

struct virtual_profile {
    pa_card_profile *parent;
    pa_card_profile *extension;
    virtual_profile_event_cb event_cb;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_droid_profile_set *profile_set;

    pa_droid_hw_module *hw_module;
    pa_droid_card_data card_data;

    pa_droid_profile *old_profile;
    pa_source *voicecall_source;

#ifdef HAVE_UDEV
    pa_droid_extcon *extcon;
#endif

    bool voice_source_routing;

    pa_modargs *modargs;
    pa_card *card;
};

struct profile_data {
    pa_droid_profile *profile;
    audio_mode_t mode;
    bool virtual_profile;
    /* Variables for virtual profiles: */
    struct virtual_profile virtual;
};

static void add_disabled_profile(pa_hashmap *profiles) {
    pa_card_profile *cp;
    struct profile_data *d;

    cp = pa_card_profile_new("off", _("Off"), sizeof(struct profile_data));

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = NULL;

    pa_hashmap_put(profiles, cp->name, cp);
}

/* Special profile for calls */
static pa_card_profile* add_virtual_profile(struct userdata *u, const char *name, const char *description,
                                            audio_mode_t audio_mode, virtual_profile_event_cb event_cb,
                                            pa_available_t available, pa_card_profile *extension_to,
                                            pa_hashmap *profiles) {
    pa_droid_profile *ap;
    pa_card_profile *cp;
    struct profile_data *d, *ext;

    pa_assert(u);
    pa_assert(u->profile_set);

    pa_log_debug("New virtual profile: %s", name);

    ap = pa_xnew0(pa_droid_profile, 1);
    ap->profile_set = u->profile_set;
    ap->name = pa_xstrdup(name);
    ap->description = pa_xstrdup(description);
    ap->priority = 50;

    pa_hashmap_put(u->profile_set->profiles, ap->name, ap);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    cp->available = available;
    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;
    d->virtual_profile = true;
    d->mode = audio_mode;
    d->virtual.event_cb = event_cb;
    d->virtual.extension = NULL;
    if (extension_to) {
        ext = PA_CARD_PROFILE_DATA(extension_to);
        ext->virtual.extension = cp;
        d->virtual.parent = extension_to;
    } else
        d->virtual.parent = NULL;

    pa_hashmap_put(profiles, cp->name, cp);

    return cp;
}

static void set_parameters_cb(pa_droid_card_data *card_data, const char *str) {
    struct userdata *u;

    pa_assert(card_data);
    pa_assert(str);

    u = card_data->userdata;

    if (u) {
        pa_droid_hw_module_lock(u->hw_module);
        u->hw_module->device->set_parameters(u->hw_module->device, str);
        pa_droid_hw_module_unlock(u->hw_module);
    }
}

static void set_card_name(pa_modargs *ma, pa_card_new_data *data, const char *module_id) {
    const char *tmp;
    char *name;

    pa_assert(ma);
    pa_assert(data);
    pa_assert(module_id);

    if ((tmp = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_card_new_data_set_name(data, tmp);
        data->namereg_fail = true;
        return;
    }

    name = pa_sprintf_malloc("droid_card.%s", module_id);
    pa_card_new_data_set_name(data, name);
    pa_xfree(name);
    data->namereg_fail = false;
}

static void add_profile(struct userdata *u, pa_hashmap *h, pa_hashmap *ports, pa_droid_profile *ap) {
    pa_card_profile *cp;
    struct profile_data *d;

    pa_assert(u);
    pa_assert(h);
    pa_assert(ports);
    pa_assert(ap);

    pa_log_debug("Card profile %s", ap->name);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    cp->available = PA_AVAILABLE_YES;
    cp->priority = ap->priority;

    cp->n_sinks = 1;
    pa_droid_add_card_ports(cp, ports, ap->output, u->core);
    cp->max_sink_channels = popcount(ap->output->output->channel_masks);
    if (ap->input) {
        pa_droid_add_card_ports(cp, ports, ap->input, u->core);
        cp->n_sources = 1;
        cp->max_source_channels = popcount(ap->input->input->channel_masks);
    }

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;
    d->virtual_profile = false;
    d->mode = AUDIO_MODE_NORMAL;

    pa_hashmap_put(h, cp->name, cp);
}

static void add_profiles(struct userdata *u, pa_hashmap *h, pa_hashmap *ports) {
    void *state;
    pa_droid_profile *ap;

    pa_assert(u);
    pa_assert(h);
    pa_assert(ports);

    PA_HASHMAP_FOREACH(ap, u->profile_set->profiles, state) {
        add_profile(u, h, ports, ap);
    }
}

static void init_profile(struct userdata *u) {
    pa_droid_mapping *am;
    struct profile_data *d;

    pa_assert(u);

    pa_log_debug("Init profile.");

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);

    if (d->profile && d->profile->output) {
        am = d->profile->output;
        am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, &u->card_data, 0, am, u->card);
    }

    if (d->profile && d->profile->input) {
        am = d->profile->input;
        am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, (audio_devices_t) 0, &u->card_data, am, u->card);
    }
}

static int set_mode(struct userdata *u, audio_mode_t mode) {
    int ret;
    const char *mode_str;

    pa_assert(u);
    pa_assert(u->hw_module);
    pa_assert(u->hw_module->device);

    switch (mode) {
        case AUDIO_MODE_RINGTONE:
            mode_str = "AUDIO_MODE_RINGTONE";
            break;
        case AUDIO_MODE_IN_CALL:
            mode_str = "AUDIO_MODE_IN_CALL";
            break;
        case AUDIO_MODE_IN_COMMUNICATION:
            mode_str = "AUDIO_MODE_IN_COMMUNICATION";
            break;
        default:
            mode_str = "AUDIO_MODE_NORMAL";
            break;
    }

    pa_log_debug("Set mode to %s.", mode_str);

    pa_droid_hw_module_lock(u->hw_module);
    if ((ret = u->hw_module->device->set_mode(u->hw_module->device, mode)) < 0)
        pa_log("Failed to set mode.");
    pa_droid_hw_module_unlock(u->hw_module);

    return ret;
}

static void park_profile(pa_droid_profile *dp) {
    pa_assert(dp);

    if (dp->output && dp->output->sink)
        pa_sink_set_port(dp->output->sink, PA_DROID_OUTPUT_PARKING, false);
    if (dp->input && dp->input->source)
        pa_source_set_port(dp->input->source, PA_DROID_INPUT_PARKING, false);
}

static bool voicecall_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling) {
    pa_card_profile *cp;

    pa_assert(u);
    pa_assert(p);
    pa_assert(u->old_profile);

    /* call mode specialities */
    if (enabling) {
        pa_droid_sink_set_voice_control(u->old_profile->output->sink, true);
        if (!u->voice_source_routing)
            pa_droid_source_set_routing(u->old_profile->input->source, false);
        if (u->old_profile->input->input->devices & AUDIO_DEVICE_IN_VOICE_CALL &&
            (cp = pa_hashmap_get(u->card->profiles, VOICE_RECORD_PROFILE_NAME))) {
            if (cp->available == PA_AVAILABLE_NO) {
                pa_log_debug("Enable %s profile.", VOICE_RECORD_PROFILE_NAME);
                pa_card_profile_set_available(cp, PA_AVAILABLE_YES);
            }
        }
    } else {
        pa_droid_sink_set_voice_control(u->old_profile->output->sink, false);
        if (!u->voice_source_routing)
            pa_droid_source_set_routing(u->old_profile->input->source, true);
        if (u->old_profile->input->input->devices & AUDIO_DEVICE_IN_VOICE_CALL &&
            (cp = pa_hashmap_get(u->card->profiles, VOICE_RECORD_PROFILE_NAME))) {
            if (cp->available == PA_AVAILABLE_YES) {
                pa_log_debug("Disable %s profile.", VOICE_RECORD_PROFILE_NAME);
                pa_card_profile_set_available(cp, PA_AVAILABLE_NO);
            }
        }
    }

    return true;
}

static bool voicecall_record_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling) {
    pa_queue *source_outputs = NULL;
    pa_droid_mapping *am;

    pa_assert(u);
    pa_assert(p);
    pa_assert(u->old_profile);

    am = u->old_profile->input;

    if (enabling) {
        /* don't do anything if voicecall source has already been created. */
        if (u->voicecall_source)
            return true;

        pa_log_info("Enabling voice call record.");

        if (u->old_profile->input) {
            am = u->old_profile->input;

            if (am->source) {
                source_outputs = pa_source_move_all_start(am->source, source_outputs);
                pa_droid_source_free(am->source);
                am->source = NULL;
            }
        }

        u->voicecall_source = pa_droid_source_new(u->module, u->modargs, __FILE__, AUDIO_DEVICE_IN_VOICE_CALL, &u->card_data, am, u->card);
        if (!u->voicecall_source)
            pa_log("Failed to enable voice call recording.");

        if (u->voicecall_source && source_outputs) {
            pa_source_move_all_finish(u->voicecall_source, source_outputs, false);
            source_outputs = NULL;
        }

    } else {
        /* don't do anything if voicecall source has already been destroyed. */
        if (!u->voicecall_source)
            return true;

        pa_log_info("Disabling voice call record.");

        source_outputs = pa_source_move_all_start(u->voicecall_source, source_outputs);
        pa_droid_source_free(u->voicecall_source);
        u->voicecall_source = NULL;

        if (u->old_profile->input) {
            am = u->old_profile->input;

            if (!am->source)
                am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, (audio_devices_t) 0, &u->card_data, am, u->card);

            if (source_outputs && am->source) {
                pa_source_move_all_finish(am->source, source_outputs, false);
                source_outputs = NULL;
            }
        }
    }

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return true;
}

static void leave_virtual_profile(struct userdata *u, pa_card *c, pa_card_profile *cp, pa_card_profile *new_profile) {
    struct profile_data *pd, *parent;

    pa_assert(u);
    pa_assert(c);
    pa_assert(cp);
    pa_assert_se((pd = PA_CARD_PROFILE_DATA(cp)));

    if (pd->virtual.parent)
        pa_log_debug("Leaving extension %s.", cp->name);

    /* First run event for old virtual profile, unless new profile is extension
     * to the old profile. */
    if (pd->virtual.extension) {
        if (pd->virtual.extension != new_profile && pd->virtual.event_cb)
            pd->virtual.event_cb(u, pd->profile, false);
    } else {
        if (pd->virtual.event_cb)
            pd->virtual.event_cb(u, pd->profile, false);
    }

    /* If old virtual profile was extension, and new profile is not extensions parent, run event
     * for extension's parent as well. */
    if (pd->virtual.parent != new_profile && pd->virtual.parent) {
        parent = PA_CARD_PROFILE_DATA(pd->virtual.parent);

        if (parent->virtual.event_cb)
            parent->virtual.event_cb(u, parent->profile, false);
    }
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    pa_droid_mapping *am;
    struct profile_data *nd, *od;
    pa_queue *sink_inputs = NULL, *source_outputs = NULL;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    if (new_profile->available != PA_AVAILABLE_YES) {
        pa_log("Profile %s is not available.", new_profile->name);
        return -1;
    }

    nd = PA_CARD_PROFILE_DATA(new_profile);
    od = PA_CARD_PROFILE_DATA(c->active_profile);

    if (nd->virtual_profile) {
        /* old_profile should always be real profile. */
        if (u->old_profile == NULL) {
            pa_assert(!od->virtual_profile);
            u->old_profile = od->profile;
        }

        if (od->virtual_profile)
            leave_virtual_profile(u, c, c->active_profile, new_profile);

        park_profile(od->profile);

        if (nd->mode != od->mode)
            set_mode(u, nd->mode);

        if (od->virtual.parent != new_profile && nd->virtual.event_cb)
            nd->virtual.event_cb(u, nd->profile, true);

        return 0;
    }

    if (od->virtual_profile) {
        pa_assert(u->old_profile);

        if (od->virtual_profile)
            leave_virtual_profile(u, c, c->active_profile, NULL);

        park_profile(nd->profile);

        if (nd->mode != od->mode)
            set_mode(u, nd->mode);

        /* If new profile is the same as from which we switched to
         * virtual profile, transfer ownership back to that profile.
         * Otherwise destroy sinks & sources and switch to new profile. */
        if (nd->profile == u->old_profile) {
            u->old_profile = NULL;
            return 0;
        } else {
            od->profile = u->old_profile;
            u->old_profile = NULL;

            /* Continue to sink-input/source-output transfer below. */
        }
    }

    /* If there are connected sink inputs/source outputs in old profile's sinks/sources move
     * them all to new sinks/sources. */

    if (od->profile && od->profile->output) {
        do {
            am = od->profile->output;

            if (!am->sink)
                continue;

            if (nd->profile && nd->profile->output && am == nd->profile->output)
                continue;

            sink_inputs = pa_sink_move_all_start(am->sink, sink_inputs);
            pa_droid_sink_free(am->sink);
            am->sink = NULL;
        } while(0);
    }

    if (od->profile && od->profile->input) {
        do {
            am = od->profile->input;

            if (!am->source)
                continue;

            if (nd->profile && nd->profile->input && am == nd->profile->input)
                continue;

            source_outputs = pa_source_move_all_start(am->source, source_outputs);
            pa_droid_source_free(am->source);
            am->source = NULL;
        } while(0);
    }

    if (nd->profile && nd->profile->output) {
        am = nd->profile->output;

        if (!am->sink)
            am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, &u->card_data, 0, am, u->card);

        if (sink_inputs && am->sink) {
            pa_sink_move_all_finish(am->sink, sink_inputs, false);
            sink_inputs = NULL;
        }
    }

    if (nd->profile && nd->profile->input) {
        am = nd->profile->input;

        if (!am->source)
            am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, (audio_devices_t) 0, &u->card_data, am, u->card);

        if (source_outputs && am->source) {
            pa_source_move_all_finish(am->source, source_outputs, false);
            source_outputs = NULL;
        }
    }

    if (sink_inputs)
        pa_sink_move_all_fail(sink_inputs);

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return 0;
}


int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    pa_card_new_data data;
    pa_droid_config_audio *config = NULL;
    const char *module_id;
    bool namereg_fail = false;
    bool voice_source_routing = false;
    pa_card_profile *virtual;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module argumets.");
        goto fail;
    }

    struct userdata *u = pa_xnew0(struct userdata, 1);
    u->core = m->core;

    if (!(config = pa_droid_config_load(ma)))
        goto fail;

    if (pa_modargs_get_value_boolean(ma, "voice_source_routing", &voice_source_routing) < 0) {
        pa_log("Failed to parse voice_source_routing argument.");
        goto fail;
    }
    u->voice_source_routing = voice_source_routing;

    module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    /* Ownership of config transfers to hw_module if opening of hw module succeeds. */
    if (!(u->hw_module = pa_droid_hw_module_get(u->core, config, module_id)))
        goto fail;

    u->card_data.set_parameters = set_parameters_cb;
    u->card_data.module_id = pa_xstrdup(module_id);
    u->card_data.userdata = u;

    u->profile_set = pa_droid_profile_set_new(u->hw_module->enabled_module);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;

    set_card_name(ma, &data, u->hw_module->module_id);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_card_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    add_profiles(u, data.profiles, data.ports);

    if (pa_hashmap_isempty(data.profiles)) {
        pa_log("Failed to find a working profile.");
        pa_card_new_data_done(&data);
        goto fail;
    }

    virtual =
    add_virtual_profile(u, VOICE_CALL_PROFILE_NAME, VOICE_CALL_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_RECORD_PROFILE_NAME, VOICE_RECORD_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_record_profile_event_cb,
                        PA_AVAILABLE_NO, virtual, data.profiles);
    add_virtual_profile(u, COMMUNICATION_PROFILE_NAME, COMMUNICATION_PROFILE_DESC,
                        AUDIO_MODE_IN_COMMUNICATION, NULL,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, RINGTONE_PROFILE_NAME, RINGTONE_PROFILE_DESC,
                        AUDIO_MODE_RINGTONE, NULL,
                        PA_AVAILABLE_YES, NULL, data.profiles);

    add_disabled_profile(data.profiles);

    pa_proplist_sets(data.proplist, PROP_DROID_HW_MODULE, u->hw_module->module_id);

    u->card = pa_card_new(m->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log("Couldn't create card.");
        goto fail;
    }

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    u->modargs = ma;
    u->module = m;

    m->userdata = u;

    init_profile(u);

#ifdef HAVE_UDEV
    u->extcon = pa_droid_extcon_new(m->core, u->card);
#endif

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(config);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if ((u = m->userdata)) {

        if (u->card && u->card->sinks)
            pa_idxset_remove_all(u->card->sinks, (pa_free_cb_t) pa_droid_sink_free);

        if (u->card && u->card->sources)
            pa_idxset_remove_all(u->card->sources, (pa_free_cb_t) pa_droid_source_free);

#ifdef HAVE_UDEV
        if (u->extcon)
            pa_droid_extcon_free(u->extcon);
#endif

        if (u->card)
            pa_card_free(u->card);

        if (u->modargs)
            pa_modargs_free(u->modargs);

        if (u->profile_set)
            pa_droid_profile_set_free(u->profile_set);

        if (u->card_data.module_id)
            pa_xfree(u->card_data.module_id);

        if (u->hw_module)
            pa_droid_hw_module_unref(u->hw_module);

        pa_xfree(u);
    }
}
