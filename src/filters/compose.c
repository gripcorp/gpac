/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2018
 *					All rights reserved
 *
 *  This file is part of GPAC / compositor filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/config_file.h>
#include <gpac/internal/compositor_dev.h>
//to set caps in filter session, to cleanup!
#include "../filter_core/filter_session.h"


GF_Err compose_bifs_dec_config_input(GF_Scene *scene, GF_FilterPid *pid, u32 oti, Bool is_remove);
GF_Err compose_bifs_dec_process(GF_Scene *scene, GF_FilterPid *pid);

GF_Err compose_odf_dec_config_input(GF_Scene *scene, GF_FilterPid *pid, u32 oti, Bool is_remove);
GF_Err compose_odf_dec_process(GF_Scene *scene, GF_FilterPid *pid);

#define COMPOSITOR_MAGIC	GF_4CC('c','o','m','p')
//a bit ugly, used by terminal (old APIs)
GF_Compositor *gf_sc_from_filter(GF_Filter *filter)
{
	GF_Compositor *ctx = (GF_Compositor *) gf_filter_get_udta(filter);
	if (ctx->magic != COMPOSITOR_MAGIC) return NULL;
	if (ctx->magic_ptr != ctx) return NULL;

	return ctx;
}

static GF_Err compose_process(GF_Filter *filter)
{
	u32 i, count;
	s32 ms_until_next = 0;
	GF_Compositor *ctx = (GF_Compositor *) gf_filter_get_udta(filter);
	if (!ctx) return GF_BAD_PARAM;

	if (ctx->check_eos_state == 2)
		return GF_EOS;

	ctx->last_error = GF_OK;
	if (ctx->reload_config) {
		ctx->reload_config = GF_FALSE;
		gf_sc_reload_config(ctx);
	}

	count = gf_list_count(ctx->systems_pids);
	for (i=0; i<count; i++) {
		GF_FilterPacket *pck;
		GF_Err e = GF_EOS;
		GF_FilterPid *pid = gf_list_get(ctx->systems_pids, i);
		GF_ObjectManager *odm = gf_filter_pid_get_udta(pid);

		assert (odm);

		e = GF_OK;
		pck = gf_filter_pid_get_packet(pid);
		if (!pck && gf_filter_pid_is_eos(pid)) {
			e = GF_EOS;
		}
		if (pck)
			gf_filter_pid_drop_packet(pid);


		if (e==GF_EOS) {
			gf_list_rem(ctx->systems_pids, i);
			i--;
			count--;
			gf_odm_on_eos(odm, pid);
		}
	}

	gf_sc_draw_frame(ctx, GF_FALSE, &ms_until_next);

	if (!ctx->player) {
		/*remember to check for eos*/
		if (ctx->dur<0) {
			if (ctx->frame_number >= (u32) -ctx->dur)
				ctx->check_eos_state = 2;
		} else if (ctx->dur>0) {
			Double n = ctx->scene_sampled_clock;
			n /= 1000;
			if (n>=ctx->dur)
				ctx->check_eos_state = 2;
		} else if (!ctx->check_eos_state) {
			ctx->check_eos_state = 1;
		}
		if ((ctx->check_eos_state==2) || (ctx->check_eos_state && ctx->root_scene && gf_sc_check_end_of_scene(ctx, 1))) {
			gf_filter_pid_set_eos(ctx->vout);
			count = gf_filter_get_ipid_count(ctx->filter);
			for (i=0; i<count; i++) {
				GF_FilterEvent evt;
				GF_FilterPid *pid = gf_filter_get_ipid(ctx->filter, i);
				GF_FEVT_INIT(evt, GF_FEVT_PLAY, pid);
				gf_filter_pid_send_event(pid, &evt);
				GF_FEVT_INIT(evt, GF_FEVT_STOP, pid);
				gf_filter_pid_send_event(pid, &evt);

			}
			return GF_EOS;
		}
		//always repost a process task since we maye have things to draw even though no new input
		gf_filter_post_process_task(filter);
		return ctx->last_error;
	}


	//to clean up,depending on whether we use a thread to poll user inputs, etc...
	if ((u32) ms_until_next > 100)
		ms_until_next = 100;

	//ask for real-time reschedule
	gf_filter_ask_rt_reschedule(filter, ms_until_next ? ms_until_next*1000 : 1);

	return ctx->last_error;
}

static void merge_properties(GF_Compositor *ctx, GF_FilterPid *pid, u32 mtype, GF_Scene *parent_scene)
{
	const GF_PropertyValue *p;
	if (!ctx->vout) return;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
	if (!p) return;

	if (mtype==GF_STREAM_SCENE) {
		if (!parent_scene || !parent_scene->is_dynamic_scene) {
			gf_filter_pid_set_property(ctx->vout, GF_PROP_PID_URL, p);
		}
	} else if (parent_scene && parent_scene->is_dynamic_scene) {
		if (mtype==GF_STREAM_VISUAL)
			gf_filter_pid_set_property(ctx->vout, GF_PROP_PID_URL, p);
	}
}

static GF_Err compose_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_ObjectManager *odm;
	const GF_PropertyValue *prop;
	u32 mtype, codecid;
	u32 i, count;
	GF_Scene *scene = NULL;
	GF_Scene *top_scene = NULL;
	GF_Compositor *ctx = (GF_Compositor *) gf_filter_get_udta(filter);
	GF_FilterEvent evt;
	Bool in_iod = GF_FALSE;
	Bool was_dyn_scene = GF_FALSE;
	if (is_remove) {
		GF_Scene *scene;
		u32 ID=0;
		GF_ObjectManager *odm = gf_filter_pid_get_udta(pid);
		//already disconnected
		if (!odm) return GF_OK;
		ID = odm->ID;
		scene = odm->parentscene;
		if (scene && !scene->is_dynamic_scene)
			scene = NULL;
		//destroy the object
		gf_odm_disconnect(odm, 2);
		if (scene) {
			if (scene->visual_url.OD_ID == ID) {
				scene->visual_url.OD_ID = 0;
				gf_scene_regenerate(scene);
			} else if (scene->audio_url.OD_ID == ID) {
				scene->audio_url.OD_ID = 0;
				gf_scene_regenerate(scene);
			} else if (scene->text_url.OD_ID == ID) {
				scene->text_url.OD_ID = 0;
				gf_scene_regenerate(scene);
			} else if (scene->dims_url.OD_ID == ID) {
				scene->dims_url.OD_ID = 0;
				gf_scene_regenerate(scene);
			}
		}
		return GF_OK;
	}

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
	if (!prop) return GF_NOT_SUPPORTED;
	mtype = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop) return GF_NOT_SUPPORTED;
	codecid = prop->value.uint;

	GF_LOG(GF_LOG_INFO, GF_LOG_COMPOSE, ("[Compositor] Configuring PID %s\n", gf_stream_type_name(mtype)));

	odm = gf_filter_pid_get_udta(pid);
	if (odm) {
		if (mtype==GF_STREAM_SCENE) { }
		else if (mtype==GF_STREAM_OD) { }
		//change of stream type for a given object, no use case yet
		else {
			if (odm->type != mtype)
				return GF_NOT_SUPPORTED;
			if (odm->mo) {
				odm->mo->config_changed = GF_TRUE;
				if ((odm->type == GF_STREAM_VISUAL) && odm->parentscene && odm->parentscene->is_dynamic_scene) {
					gf_scene_force_size_to_video(odm->parentscene, odm->mo);
					odm->mo->config_changed = GF_TRUE;
				}
			}
			gf_odm_update_duration(odm, pid);
			gf_odm_check_clock_mediatime(odm);
		}
		merge_properties(ctx, pid, mtype, odm->parentscene);
		return GF_OK;
	}

	//create a default scene
	if (!ctx->root_scene) {
		const char *service_url = "unknown";
		const GF_PropertyValue *p = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
		if (p) service_url = p->value.string;
		
		ctx->root_scene = gf_scene_new(ctx, NULL);
		ctx->root_scene->is_dynamic_scene = GF_TRUE;
		ctx->root_scene->root_od = gf_odm_new();
		ctx->root_scene->root_od->scene_ns = gf_scene_ns_new(ctx->root_scene, ctx->root_scene->root_od, service_url, NULL);
		ctx->root_scene->root_od->subscene = ctx->root_scene;
	}

	//default scene is root one
	scene = ctx->root_scene;
	top_scene = ctx->root_scene;

	//browse all scene namespaces and figure out our parent scene
	count = gf_list_count(top_scene->namespaces);
	for (i=0; i<count; i++) {
		GF_SceneNamespace *sns = gf_list_get(top_scene->namespaces, i);
		if (!sns->source_filter) {
			if (sns->connect_ack && sns->owner) {
				scene = sns->owner->subscene ? sns->owner->subscene : sns->owner->parentscene;
				break;
			}
			continue;
		}
		assert(sns->owner);
		if (gf_filter_pid_is_filter_in_parents(pid, sns->source_filter)) {
			//we are attaching an inline, create the subscene if not done already
			if (!sns->owner->subscene && ((mtype==GF_STREAM_OD) || (mtype==GF_STREAM_SCENE)) ) {
				assert(sns->owner->parentscene);
				sns->owner->subscene = gf_scene_new(ctx, sns->owner->parentscene);
				sns->owner->subscene->root_od = sns->owner;
			}
			scene = sns->owner->subscene ? sns->owner->subscene : sns->owner->parentscene;
			break;
		}
	}
	assert(scene);

	was_dyn_scene = scene->is_dynamic_scene;

	//we have an MPEG-4 ESID defined for the PID, this is MPEG-4 systems
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_ESID);
	if (prop && scene->is_dynamic_scene) {
		scene->is_dynamic_scene = GF_FALSE;
	}

	//pure OCR streams are handled by dispatching OCR on the PID(s)
	if (codecid != GF_CODECID_RAW)
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_IN_IOD);
	if (prop && prop->value.boolean) {
		scene->is_dynamic_scene = GF_FALSE;
		in_iod = GF_TRUE;
	}
	if ((mtype==GF_STREAM_OD) && !in_iod) return GF_NOT_SUPPORTED;

	//we inserted a scene (bt/svg/...) after a pid (passthrough mode), we need to create a new namesapce for
	//the scene and reassign the old namespace to the previously created ODM
	if (was_dyn_scene && (was_dyn_scene != scene->is_dynamic_scene)) {
		GF_SceneNamespace *new_sns=NULL;
		const char *service_url = "unknown";
		const GF_PropertyValue *p = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
		if (p) service_url = p->value.string;
		new_sns = gf_scene_ns_new(ctx->root_scene, ctx->root_scene->root_od, service_url, NULL);

		for (i=0; i<gf_list_count(scene->resources); i++) {
			GF_ObjectManager *anodm = gf_list_get(scene->resources, i);

			if (new_sns && (anodm->scene_ns == scene->root_od->scene_ns) && (scene->root_od->scene_ns->owner==scene->root_od)) {
				scene->root_od->scene_ns->owner = odm;
			}
		}
		scene->root_od->scene_ns = new_sns;
		gf_sc_set_scene(ctx, NULL);
		gf_sg_reset(scene->graph);
		gf_sc_set_scene(ctx, scene->graph);
	}

	//setup object (clock) and playback requests
	gf_scene_insert_pid(scene, scene->root_od->scene_ns, pid, in_iod);

	if (was_dyn_scene != scene->is_dynamic_scene) {
		for (i=0; i<gf_list_count(scene->resources); i++) {
			GF_ObjectManager *anodm = gf_list_get(scene->resources, i);
			if (anodm->mo)
				anodm->flags |= GF_ODM_PASSTHROUGH;
		}
	}


	//attach scene to input filters - may be true for dynamic scene (text rendering) and regular scenes
	if ((mtype==GF_STREAM_OD) || (mtype==GF_STREAM_SCENE) ) {
		GF_FEVT_INIT(evt, GF_FEVT_ATTACH_SCENE, pid);
		evt.attach_scene.object_manager = gf_filter_pid_get_udta(pid);
		gf_filter_pid_send_event(pid, &evt);
	}
	//scene is dynamic
	if (scene->is_dynamic_scene) {
		char *sep = scene->root_od->scene_ns->url_frag;
		if (sep && ( !strnicmp(sep, "LIVE360", 7) || !strnicmp(sep, "360", 3) || !strnicmp(sep, "VR", 2) ) ) {
			scene->vr_type = 1;
		}
		gf_scene_regenerate(scene);
	}

	merge_properties(ctx, pid, mtype, scene);
	return GF_OK;
}

static GF_Err compose_reconfig_output(GF_Filter *filter, GF_FilterPid *pid)
{
	const GF_PropertyValue *p;
	u32 sr, o_fmt, nb_ch, cfg, afmt;
	Bool needs_reconfigure = GF_FALSE;
	GF_Compositor *ctx = (GF_Compositor *) gf_filter_get_udta(filter);

	if (ctx->vout == pid) {
		u32 w, h;
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_PIXFMT);
		if (p) {
			ctx->opfmt = p->value.uint;
			gf_filter_pid_set_property(ctx->vout, GF_PROP_PID_PIXFMT, &PROP_UINT(ctx->opfmt) );
		}
		
		w = h = 0;
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_WIDTH);
		if (p) w = p->value.uint;
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_HEIGHT);
		if (p) h = p->value.uint;

		if (w && h) {
			ctx->size.x = w;
			ctx->size.y = h;
			gf_filter_pid_set_property(ctx->vout, GF_PROP_PID_WIDTH, &PROP_UINT(w) );
			gf_filter_pid_set_property(ctx->vout, GF_PROP_PID_HEIGHT, &PROP_UINT(h) );
		}
		return GF_OK;
	}

	if (ctx->audio_renderer->aout == pid) {

		gf_mixer_get_config(ctx->audio_renderer->mixer, &sr, &nb_ch, &o_fmt, &cfg);
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_SAMPLE_RATE);
		if (p && (p->value.uint != sr)) {
			sr = p->value.uint;
			needs_reconfigure = GF_TRUE;
		}
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_NUM_CHANNELS);
		if (p && (p->value.uint != nb_ch)) {
			nb_ch = p->value.uint;
			needs_reconfigure = GF_TRUE;
		}
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_AUDIO_FORMAT);
		if (p) afmt = p->value.uint;
		else afmt = GF_AUDIO_FMT_S16;

		if (o_fmt != afmt) {
			needs_reconfigure = GF_TRUE;
		}
		if (!needs_reconfigure) return GF_OK;

		GF_LOG(GF_LOG_INFO, GF_LOG_AUDIO, ("[Compositor] Audio output caps negotiated to %d Hz %d channels %s \n", sr, nb_ch, gf_audio_fmt_name(afmt) ));
		gf_mixer_set_config(ctx->audio_renderer->mixer, sr, nb_ch, afmt, 0);
		ctx->audio_renderer->need_reconfig = GF_TRUE;
		return GF_OK;
	}
	return GF_NOT_SUPPORTED;
}

static Bool compose_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	switch (evt->base.type) {
	//event(s) we trigger on ourselves to go up the filter chain
	case GF_FEVT_CAPS_CHANGE:
		return GF_FALSE;
	case GF_FEVT_CONNECT_FAIL:
	{
		GF_Compositor *ctx = (GF_Compositor *) gf_filter_get_udta(filter);
		if (ctx->audio_renderer && (evt->base.on_pid == ctx->audio_renderer->aout))
			ctx->audio_renderer->non_rt_output = GF_FALSE;
	}
		return GF_FALSE;
	default:
		break;
	}
	//all events cancelled (play/stop/etc...)
	return GF_TRUE;
}

static GF_Err compose_update_arg(GF_Filter *filter, const char *arg_name, const GF_PropertyValue *arg_val)
{
	GF_Compositor *compositor = gf_filter_get_udta(filter);
	compositor->reload_config = GF_TRUE;
	return GF_OK;
}

static void compose_finalize(GF_Filter *filter)
{
	GF_Compositor *ctx = gf_filter_get_udta(filter);

	if (ctx) {
		gf_sc_set_scene(ctx, NULL);
		if (ctx->root_scene) {
			gf_odm_disconnect(ctx->root_scene->root_od, GF_TRUE);
		}
		gf_sc_unload(ctx);
	}
}

GF_Err compose_initialize(GF_Filter *filter)
{
	GF_Err e;
	GF_FilterSessionCaps sess_caps;
	GF_FilterPid *pid;
	GF_Compositor *ctx = gf_filter_get_udta(filter);

	ctx->magic = COMPOSITOR_MAGIC;
	ctx->magic_ptr = (void *) ctx;

	e = gf_sc_load(ctx);
	if (e) return e;

	ctx->filter = filter;

	gf_filter_get_session_caps(filter, &sess_caps);

	sess_caps.max_screen_width = ctx->video_out->max_screen_width;
	sess_caps.max_screen_height = ctx->video_out->max_screen_height;
	sess_caps.max_screen_bpp = ctx->video_out->max_screen_bpp;

	gf_filter_set_session_caps(filter, &sess_caps);

	//declare audio output pid first
	if (! (ctx->init_flags & GF_TERM_NO_AUDIO) && ctx->audio_renderer) {
		pid = ctx->audio_renderer->aout = gf_filter_pid_new(filter);
		gf_filter_pid_set_udta(pid, ctx);
		gf_filter_pid_set_name(pid, "aout");
		gf_filter_pid_set_property(pid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO) );
		gf_filter_pid_set_property(pid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW) );
		gf_filter_pid_set_property(pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16) );
		gf_filter_pid_set_property(pid, GF_PROP_PID_TIMESCALE, &PROP_UINT(44100) );
		gf_filter_pid_set_property(pid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(44100) );
		gf_filter_pid_set_property(pid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(2) );
		gf_filter_pid_set_max_buffer(ctx->audio_renderer->aout, 1000*ctx->abuf);
		gf_filter_pid_set_loose_connect(pid);
	}
	
	//declare video output pid
	pid = ctx->vout = gf_filter_pid_new(filter);
	gf_filter_pid_set_name(pid, "vout");
	//compositor initiated for RT playback, vout pid may not be connected
	if (! (ctx->init_flags & GF_TERM_NO_DEF_AUDIO_OUT))
		gf_filter_pid_set_loose_connect(pid);

	gf_filter_pid_set_property(pid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW) );
	gf_filter_pid_set_property(pid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL) );
	if (ctx->timescale)
		gf_filter_pid_set_property(pid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale) );
	else
		gf_filter_pid_set_property(pid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->fps.num) );

	gf_filter_pid_set_property(pid, GF_PROP_PID_PIXFMT, &PROP_UINT(ctx->opfmt ? ctx->opfmt : GF_PIXEL_RGB) );
	gf_filter_pid_set_property(pid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->output_width) );
	gf_filter_pid_set_property(pid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->output_height) );

	gf_filter_pid_set_property(pid, GF_PROP_PID_FPS, &PROP_FRAC(ctx->fps) );

	//always request a process task since we don't depend on input packets arrival (animations, pure scene presentations)
	gf_filter_post_process_task(filter);
	return GF_OK;
}

#define OFFS(_n)	#_n, offsetof(GF_Compositor, _n)
static GF_FilterArgs CompositorArgs[] =
{
	{ OFFS(aa), "Set anti-aliasing mode for raster graphics - whether the setting is applied or not depends on the graphics module / graphic card.\n"\
		"\tnone: no anti-aliasing\n"\
    	"\ttext: anti-aliasing for text only\n"\
    	"\tall: complete anti-aliasing", GF_PROP_UINT, "all", "none|text|all", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(hlfill), "Set highlight fill color (ARGB)", GF_PROP_UINT, "0x0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(hlline), "Set highlight stroke color (ARGB)", GF_PROP_UINT, "0xFF000000", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(hllinew), "Set highlight stroke width", GF_PROP_FLOAT, "1.0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(sz), "specifies whether scalable zoom should be used or not. When scalable zoom is enabled, resizing the output window will also recompute all vectorial objects. Otherwise only the final buffer is stretched.", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(bc), "default background color to use when displaying transparent images or video with no scene composition instructions", GF_PROP_UINT, "0", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(yuvhw), "enables YUV hardware for 2D blits", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(blitp), "partial hardware blits (if not set, will force more redraw)", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(stress), "enables stress mode of compositor (rebuild all vector graphics and texture states at each frame)", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(fast), "enables speed optimization - whether the setting is applied or not depends on the graphics module / graphic card", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(bvol), "draws bounding volume of objects.\n"\
			"\tno: disable bounding box\n"\
			"\tbox: draws a rectangle (2D) or box (3D mode)\n"\
			"\taabb: draws axis-aligned bounding-box tree (3D only)", GF_PROP_UINT, "no", "no|box|aabb", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(textxt), "specifies whether text shall be drawn to a texture and then rendered or directly rendered. Using textured text can improve text rendering in 3D and also improve text-on-video like content. Possible values are:\n"\
		"\tdefault: use texturing for OpenGL rendering, no texture for 2D rasterizer\n"\
		"\tnever: never uses text textures\n"\
		"\talways: always render text to texture before drawing\n"\
		"", GF_PROP_UINT, "default", "default|never|always", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(out8b), "converts 10-bit video to 8 bit texture before GPU upload.", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(drop), "drops late frame when drawing. By default frames are not droped until a heavy desync of 1 sec is observed", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(sclock), "forces synchronizing all streams on a single clock", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(sgaze), "simulate gaze events through mouse", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(ckey), "color key to use in windowless mode (0xFFRRGGBB). GPAC currently doesn't support true alpha blitting to desktop due to limitations in most windowing toolkit, it therefore uses color keying mechanism. The alpha part of the key is used for global transparency of GPAC's output, if supported.", GF_PROP_UINT, "0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(timeout), "timeout in ms after which a source is considered dead", GF_PROP_UINT, "10000", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(fps), "simulation frame rate when animation-only sources are played (ignored when video is present).", GF_PROP_FRACTION, "30/1", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(timescale), "timescale used for output packets when no input video pid. A value of 0 means fps numerator.", GF_PROP_UINT, "0", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(autofps), "uses video input fps for output. If no video or not set, uses fps option. Ignored in player mode", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(vfr), "only emits frames when changes are detected. Always true in player mode", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},

	{ OFFS(dur), "duration of generation. Mostly used when no video input is present. Negative values mean number of frames, positive values duration in second, 0 stops as soon as all streams are done.", GF_PROP_DOUBLE, "0", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(fsize), "Forces the scene to resize to the biggest bitmap available if no size info is given in the BIFS configuration", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(mode2d), "specifies whether immediate drawing should be used or not. Possible values are:\n"\
	"\timmediate: the screen is completely redrawn at each frame (always on if passthrough mode is detected).\n"\
	"\tdefer: object positioning is tracked from frame to frame and dirty rectangles info is collected in order to redraw the minimal amount of the screen buffer.\n"\
	"\tdebug: only renders changed areas, reseting other areas\n"\
	 "Whether the setting is applied or not depends on the graphics module and player mode.", GF_PROP_UINT, "defer", "defer|immediate|debug", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(amc), "audio multichannel support; if disabled always downmix to stereo. usefull if the multichannel output doesn't work properly", GF_PROP_BOOL, "true", NULL, 0},
	{ OFFS(asr), "forces output sample rate - 0 for auto", GF_PROP_UINT, "0", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(ach), "forces output channels - 0 for auto", GF_PROP_UINT, "0", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(alayout), "forces output channel layout - 0 for auto", GF_PROP_UINT, "0", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(afmt), "forces output channel format - 0 for auto", GF_PROP_PCMFMT, "s16", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(asize), "audio output packet size in samples", GF_PROP_UINT, "1024", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(abuf), "audio output buffer duration in ms - the audio renderer fills the output pid up to this value. A too low value will lower latency but can have real-time playback issues", GF_PROP_UINT, "50", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(avol), "audio volume in percent", GF_PROP_UINT, "100", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(apan), "audio pan in percent, 50 is no pan", GF_PROP_UINT, "50", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(async), "audio resynchronization; if disabled, audio data is never dropped but may get out of sync", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},

	{ OFFS(buf), "playout buffer in ms. Overriden by BufferLenth property of input pid", GF_PROP_UINT, "3000", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(rbuf), "rebuffer trigger in ms. Overriden by RebufferLenth property of input pid", GF_PROP_UINT, "1000", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(mbuf), "max buffer in ms (must be greater than playout buffer). Overriden by BufferMaxOccupancy property of input pid", GF_PROP_UINT, "3000", NULL, GF_FS_ARG_UPDATE},

#ifndef GPAC_DISABLE_3D
	{ OFFS(ogl), "specifies 2D rendering mode.\nPossible values are:\n"\
				"\tauto: automaitically decides betwwen on, off and hybrid based on content.\n"\
				"\toff: disables OpenGL - 3D won't be rendered.\n"\
				"\ton: uses OpenGL for all graphics - this will involve polygon tesselation and 2D graphics will not look as nice as 2D mode.\n"\
				"\thybrid: the compositor performs software drawing of 2D graphics with no textures (better quality) and uses OpenGL for all textures.\n"\
				, GF_PROP_UINT, "auto", "auto|off|hybrid|on|raster", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(pbo), "enables PixelBufferObjects to push YUV textures to GPU in OpenGL Mode. This may slightly increase the performances of the playback.", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(nav), "overrides the default navigation mode of MPEG-4/VRML (Walk) and X3D (Examine)", GF_PROP_UINT, "none", "none|walk|fly|pan|game|slide|exam|orbit|vr", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(linegl), "specifies that outlining shall be done through OpenGL pen width rather than vectorial outlining", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(epow2), "emulate power-of-2 textures for openGL (old hardware). Ignored if OpenGL rectangular texture extension is enabled\n"\
	"\tyes: video texture is not resized but emulated with padding. This usually speeds up video mapping on shapes but disables texture transformations\n"\
	"\tno: video is resized to a power of 2 texture when mapping to a shape", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(paa), "specifies whether polygon antialiasing should be used in full antialiasing mode. If not set, only lines and points antialiasing are used.", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(bcull), "specifies whether backface culling shall be disable or not. Possible values are:\n"\
				"\ton: enables backface culling\n"\
				"\toff: disables backface culling\n"\
				"\talpha: only enables backface culling for transparent meshes\n"\
		"", GF_PROP_UINT, "on", "off|on|alpha", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(wire), "enables wireframe mode.\n"\
	"\"none\": objects are drawn as solid\n"\
    "\"only\": objects are drawn as wireframe only\n"\
    "\"solid\": objects are drawn as solid and wireframe is then drawn", GF_PROP_UINT, "none", "none|only|solid", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(norms), "enables normal drawing", GF_PROP_UINT, "none", "none|face|vertex", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(glus), "use GLU scale, which may be slower but nicer than GPAC's software stretch routines.", GF_PROP_BOOL,
#ifdef GPAC_CONFIG_IOS
	"false",
#else
	"true",
#endif
	 NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(rext), "use non power of two (rectangular) texture GL extension", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(cull), "use aabb culling: large objects are rendered in multiple calls when not fully in viewport", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(yuvgl), "enables YUV open GL pixel format support if hardware supports it. If not supported and shaders are available, YUV to RGB will be done on GPU", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(depth_gl_scale), "sets depth scaler", GF_PROP_FLOAT, "100", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(depth_gl_type), "sets geometry type used to draw depth video", GF_PROP_UINT, "vbo", "none|point|strip|vbo", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(views), "specifies the number of views to use in stereo mode", GF_PROP_UINT, "0", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(stereo), "specifies the stereo output type (default \"none\"). If your graphic card does not support OpenGL shaders, only \"top\" and \"side\" modes will be available.\n"\
		"\"side\": images are displayed side by side from left to right.\n"\
		"\"top\": images are displayed from top (laft view) to bottom (right view).\n"\
		"\"hmd\": same as side except that view aspect ratio is not changed.\n"\
		"\"ana\": standard color anaglyph (red for left view, green and blue for right view) is used (forces views=2).\n"\
		"\"cols\": images are interleaved by columns, left view on even columns and left view on odd columns (forces views=2).\n"\
		"\"rows\": images are interleaved by columns, left view on even rows and left view on odd rows (forces views=2).\n"\
		"\"spv5\": images are interleaved by for SpatialView 19'' 5 views display, fullscreen mode (forces views=5).\n"\
		"\"alio8\": images are interleaved by for Alioscopy 8 views displays, fullscreen mode (forces views=8).\n"\
		"\"custom\": images are interleaved according to the shader file indicated in mvsgader option. The shader is exposed each view as uniform sampler2D gfViewX, where X is the view number starting from the left", GF_PROP_UINT, "none", "none|top|side|hmd|custom|cols|rows|ana|spv5|alio8", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(mvshader), "indicates file path to the custom multiview interleaving shader", GF_PROP_STRING, NULL, NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(fpack), "indicates default frame packing of input video", GF_PROP_UINT, "none", "none|top|side", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(camlay), "sets camera layout in multiview modes", GF_PROP_UINT, "offaxis", "straight|offaxis|linear|circular", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(iod), "specifies the inter-occular distance (eye separation) in cm (distance between the cameras). ", GF_PROP_FLOAT, "6.4", NULL, GF_FS_ARG_UPDATE},
	{ OFFS(rview), "reverse view order", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},

	{ OFFS(tvtn), "number of point sampling for tile visibility algo", GF_PROP_UINT, "30", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(tvtt), "number of points above which the tile is considered visible", GF_PROP_UINT, "0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(tvtd), "disables the tile having full coverage of the SRD, only displaying partial tiles", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(tvtf), "force all tiles to be considered visible, regardless of viewpoint", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(fov), "default field of view for VR", GF_PROP_FLOAT, "1.570796326794897", NULL, GF_FS_ARG_UPDATE},

#endif

#ifdef GF_SR_USE_DEPTH
	{ OFFS(autocal), "auto callibrates znear/zfar in depth rendering mode", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(dispdepth), "sets display depth, negative value uses default screen height", GF_PROP_SINT, "-1", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(dispdist), "specifies the distance in cm between the camera and the zero-disparity plane. There is currently no automatic calibration of depth in GPAC", GF_PROP_FLOAT, "50", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
#ifndef GPAC_DISABLE_3D
	{ OFFS(focdist), "sets focus distance", GF_PROP_FLOAT, "0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
#endif
#endif

#ifdef GF_SR_USE_VIDEO_CACHE
	{ OFFS(vcsize), "sets cache size when storing raster graphics to memory", GF_PROP_UINT, "0", "0,+I", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(vcscale), "sets cache scale factor in percent when storing raster graphics to memory", GF_PROP_UINT, "100", "0,100", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(vctol), "sets cache tolerance when storing raster graphics to memory. If the difference between the stored version scale and the target display scale is less than tolerance, the cache will be used, otherwise it will be recomputed", GF_PROP_UINT, "30", "0,100", GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
#endif
	{ OFFS(wfont), "forces to wait for SVG fonts to be loaded before displaying frames", GF_PROP_FLOAT, "0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(size), "force output size. If not set, size is derived from inputs", GF_PROP_VEC2I, "0x0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(dpi), "default dpi if not indicated by video output", GF_PROP_VEC2I, "96x96", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(dbgpvr), "debug scene used by PVR addon", GF_PROP_FLOAT, "0", NULL, GF_FS_ARG_UPDATE|GF_FS_ARG_HINT_EXPERT},
	{ OFFS(player), "sets compositor in player mode, see filter help", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(opfmt), "pixel format to use for output. Ignored in player mode", GF_PROP_PIXFMT, "none", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(drv), "indicates if graphics driver should be used. Ignored in player mode. Allowed values are:\n"\
				"\tno: never loads a graphics driver (software blitting used, no 3D possible)\n"\
				"\tyes: always loads a graphics driver. Output pixel format will be RGB.\n"\
				"\tauto: decides based on the loaded content.\n"\
			, GF_PROP_UINT, "auto", "no|yes|auto", GF_FS_ARG_HINT_EXPERT},
	{0}
};

static const GF_FilterCapability CompositorCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_SCENE),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_OD),
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_UINT(GF_CAPS_INPUT_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_INPUT_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_UINT(GF_CAPS_INPUT_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

const GF_FilterRegister CompositorFilterRegister = {
	.name = "compositor",
	GF_FS_SET_DESCRIPTION("GPAC interactive media compositor")
	GF_FS_SET_HELP("The GPAC compositor allows mixing audio, video, text and graphics in a timed fashion.\n"\
	"The filter is explicit only and is never loaded during link resolution.\n"
	"The compositor operates either in player or non-player mode.\n"\
	"\n"\
	"In player mode, the compositor acts as a pseudo-sink for the video side and creates its own output window.\n"\
	"The video frames are dispatched to the output video pid in the form of frame pointers requiring later GPU read if used.\n"\
	"The audio part acts as a regular filter.\n"\
	"\n"\
	"In non-player mode, the compositor acts as a regular filter generating frames based on the scene loaded.\n"\
	"It will generate its outputs based on the input video frames.\n"\
	"If no input video frames (e.g. pure BIFS / SVG / VRML), the filter will generate frames based on the fps option, at constant or variable frame rate.\n"\
	"It will stop generating frames as soon as all input streams are done, unless extended/reduced by the dur option.\n"
	"In non-player mode, the special URL 'gpid://' is used to locate PIDs in the scene description, in order to design scenes independently from source media.\n"\
	"When such a pid is associated to a Background2D node in BIFS, the compositor operates in passthrough mode.\n"\
	"In this mode, only new input frames on the passthrough pid will generate new frames, and the scene clock matches the input packet time.\n"\
	"The output size and pixel format will be set to the input size and pixel format, unless specified otherwise in the filter options.\n"\
	"\n"\
	"If only 2D graphics are used and display driver is not forced, 2D rasterizer will happen in the output pixel format (including YUV pixel formats).\n"\
	"In this case, inplace processing (rasterizing over the input frame data) will be used whenever allowed by input data.\n"\
	"\n"\
	"If 3D graphics are used or display driver is forced, OpenGL will be used on offscreen surface and the output packet will be an OpenGL texture.\n"\
	)
	.private_size = sizeof(GF_Compositor),
	.flags = GF_FS_REG_MAIN_THREAD | GF_FS_REG_EXPLICIT_ONLY,
	.max_extra_pids = (u32) -1,
	SETCAPS(CompositorCaps),
	.args = CompositorArgs,
	.initialize = compose_initialize,
	.finalize = compose_finalize,
	.process = compose_process,
	.process_event = compose_process_event,
	.configure_pid = compose_configure_pid,
	.reconfigure_output = compose_reconfig_output,
	.update_arg = compose_update_arg
};

const GF_FilterRegister *compose_filter_register(GF_FilterSession *session)
{
	u32 i=0;
	u32 nb_args = sizeof(CompositorArgs) / sizeof(GF_FilterArgs) - 1;

	for (i=0; i<nb_args; i++) {
		if (!strcmp(CompositorArgs[i].arg_name, "afmt")) {
			CompositorArgs[i].min_max_enum = gf_audio_fmt_all_names();
			break;
		}
		else if (!strcmp(CompositorArgs[i].arg_name, "pfmt")) {
			CompositorArgs[i].min_max_enum =  gf_pixel_fmt_all_names();
		}
	}
	return &CompositorFilterRegister;
}



