/*****************************************************************************\
 *  slurm_acct_gather_energy.c - implementation-independent job energy
 *  accounting plugin definitions
 *****************************************************************************
 *  Copyright (C) 2012 Bull-HN-PHX.
 *  Written by Bull-HN-PHX/d.rusak,
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "src/common/macros.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/slurm_acct_gather_energy.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"

/*
** Define slurm-specific aliases for use by plugins, see slurm_xlator.h
** for details.
 */
//strong_alias(acct_gather_energy_destroy, slurm_acct_gather_energy_destroy);

typedef struct slurm_acct_gather_energy_ops {
	int (*update_node_energy) (void);
	int (*get_data)           (enum acct_energy_type data_type, void *data);
	int (*set_data)           (enum acct_energy_type data_type, void *data);
	void (*conf_options)      (s_p_options_t **full_options,
				   int *full_options_cnt);
	void (*conf_set)          (s_p_hashtbl_t *tbl);
	void (*conf_values)        (List *data);
} slurm_acct_gather_energy_ops_t;
/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_acct_gather_energy_ops_t.
 */
static const char *syms[] = {
	"acct_gather_energy_p_update_node_energy",
	"acct_gather_energy_p_get_data",
	"acct_gather_energy_p_set_data",
	"acct_gather_energy_p_conf_options",
	"acct_gather_energy_p_conf_set",
	"acct_gather_energy_p_conf_values",
};

static slurm_acct_gather_energy_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock =	PTHREAD_MUTEX_INITIALIZER;
static bool init_run = false;
static bool acct_shutdown = true;
static float freq = 0.0;


static void *_watch_node(void *arg)
{
	int type = PROFILE_ENERGY;
	int delta = acct_gather_profile_timer[type].freq - 1;
	while (init_run && acct_gather_profile_running) {
		/* Do this until shutdown is requested */
		(*(ops.set_data))(ENERGY_DATA_PROFILE, &delta);
		slurm_mutex_lock(&acct_gather_profile_timer[type].notify_mutex);
		pthread_cond_wait(
			&acct_gather_profile_timer[type].notify,
			&acct_gather_profile_timer[type].notify_mutex);
		slurm_mutex_unlock(&acct_gather_profile_timer[type].
				   notify_mutex);
	}

	return NULL;
}


extern int slurm_acct_gather_energy_init(void)
{
	int retval = SLURM_SUCCESS;
	char *plugin_type = "acct_gather_energy";
	char *type = NULL;

	if (init_run && g_context)
		return retval;

	slurm_mutex_lock(&g_context_lock);

	if (g_context)
		goto done;

	type = slurm_get_acct_gather_energy_type();

	g_context = plugin_context_create(
		plugin_type, type, (void **)&ops, syms, sizeof(syms));

	if (!g_context) {
		error("cannot create %s context for %s", plugin_type, type);
		retval = SLURM_ERROR;
		goto done;
	}
	init_run = true;

done:
	slurm_mutex_unlock(&g_context_lock);
	if (retval == SLURM_SUCCESS)
		retval = acct_gather_conf_init();
	if (retval != SLURM_SUCCESS)
	fatal("can not open the %s plugin", type);
	xfree(type);

	return retval;
}

extern int acct_gather_energy_fini(void)
{
	int rc;

	if (!g_context)
		return SLURM_SUCCESS;

	init_run = false;
	rc = plugin_context_destroy(g_context);
	g_context = NULL;

	return rc;
}

extern acct_gather_energy_t *acct_gather_energy_alloc(uint16_t cnt)
{
	int i;
	acct_gather_energy_t *energy =
		xmalloc(sizeof(struct acct_gather_energy) * cnt);

	///* TODO verify: xmallox should zero-fill allocations, but let's be certain
	// * before removing the following statement. */
	//for (i = 0; i < cnt; i++) {
	//	energy[i].gpu_watts = NULL;
	//}

	return energy;
}

extern void acct_gather_energy_destroy(acct_gather_energy_t *energy)//, char *callfunc)
{
	//if (energy->gpu_watts) {
	//	//error("Caller <%s> freeing %d", callfunc, energy->gpu_watts);
	//	xfree(energy->gpu_watts);
	//}
	xfree(energy);
}

//#define acct_gather_energy_destroy(x) _acct_gather_energy_destroy((x), __func__)


extern void acct_gather_energy_pack(acct_gather_energy_t *energy, Buf buffer,
				    uint16_t protocol_version)
{
	if (protocol_version >= SLURM_15_08_PROTOCOL_VERSION) {
		if (!energy) {
			pack64(0, buffer);
			pack32(0, buffer);
			pack64(0, buffer);
			pack32(0, buffer);
			pack64(0, buffer);
			pack32(0, buffer);
			pack_time(0, buffer);
			return;
		}

		pack64(energy->base_consumed_energy, buffer);
		pack32(energy->base_watts, buffer);
		pack64(energy->consumed_energy, buffer);
		pack32(energy->current_watts, buffer);
		pack64(energy->previous_consumed_energy, buffer);
		/* GPUs */
		//error("packing: energy->num_gpus: %d", energy->num_gpus);
		//error("packing: energy->gpu_watts: %d", energy->gpu_watts);int i;
		//if (energy->num_gpus) {// && energy->gpu_watts) {
			//for(i=0;i<energy->num_gpus;i++)
			//	error("packing: energy->gpu_watts[%d]: %llu", i, energy->gpu_watts[i]);
			pack32(energy->num_gpus, buffer);
			int i; for(i = 0; i < MAX_GPUS; i++) {
				pack32(energy->gpu_watts[i], buffer);}
			//pack32_array(energy->gpu_watts, MAX_GPUS, buffer);
		//}
		//else {
		//	//0 gpus
		//	pack32(0, buffer);
		//}
		/* END GPUs*/
		pack_time(energy->poll_time, buffer);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		if (!energy) {
			int i;
			for (i=0; i<5; i++)
				pack32(0, buffer);
			pack_time(0, buffer);
			return;
		}

		pack32((uint32_t) energy->base_consumed_energy, buffer);
		pack32(energy->base_watts, buffer);
		pack32((uint32_t) energy->consumed_energy, buffer);
		pack32(energy->current_watts, buffer);
		pack32((uint32_t) energy->previous_consumed_energy, buffer);
		pack_time(energy->poll_time, buffer);
	}
}

extern int acct_gather_energy_unpack(acct_gather_energy_t **energy, Buf buffer,
				     uint16_t protocol_version, bool need_alloc)
{
	uint32_t uint32_tmp;
	acct_gather_energy_t *energy_ptr;

	//grow_buf(buffer, BUF_SIZE);

	if (need_alloc) {
		energy_ptr = acct_gather_energy_alloc(1);
		*energy = energy_ptr;
	} else {
		energy_ptr = *energy;
	}

	// TODO check num_gpus before malloc?

	if (protocol_version >= SLURM_15_08_PROTOCOL_VERSION) {
		safe_unpack64(&energy_ptr->base_consumed_energy, buffer);
		safe_unpack32(&energy_ptr->base_watts, buffer);
		safe_unpack64(&energy_ptr->consumed_energy, buffer);
		safe_unpack32(&energy_ptr->current_watts, buffer);
		safe_unpack64(&energy_ptr->previous_consumed_energy, buffer);
		safe_unpack32(&energy_ptr->num_gpus, buffer);

		/*if (!energy_ptr->gpu_watts) {
			energy_ptr->gpu_watts = xmalloc(energy_ptr->num_gpus * sizeof(uint64_t));
			if(!energy_ptr->gpu_watts) {
				error("energy_unpack: could not allocate memory");
				energy_ptr->gpu_watts = 0;
			}
		}*/

		//if(energy_ptr->num_gpus) {
		int i;for(i = 0; i < MAX_GPUS; i++)
			unpack32(&energy_ptr->gpu_watts[i], buffer);
		//}
		error("unpacking: energy_ptr->num_gpus: %d", energy_ptr->num_gpus);
		error("unpacking: energy_ptr->gpu_watts: %d", energy_ptr->gpu_watts[0]);
		//if (energy_ptr->num_gpus && energy_ptr->gpu_watts) {
		//	for(i = 0; i < energy_ptr->num_gpus; i++)
		//		error("unpacking: energy_ptr->gpu_watts[%d]: %llu", i, energy_ptr->gpu_watts[i]);
		//}
		safe_unpack_time(&energy_ptr->poll_time, buffer);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		safe_unpack32(&uint32_tmp, buffer);
		energy_ptr->base_consumed_energy = (uint64_t) uint32_tmp;
		safe_unpack32(&energy_ptr->base_watts, buffer);
		safe_unpack32(&uint32_tmp, buffer);
		energy_ptr->consumed_energy = (uint64_t) uint32_tmp;
		safe_unpack32(&energy_ptr->current_watts, buffer);
		safe_unpack32(&uint32_tmp, buffer);
		energy_ptr->previous_consumed_energy = (uint64_t) uint32_tmp;
		safe_unpack_time(&energy_ptr->poll_time, buffer);
	}

	return SLURM_SUCCESS;

unpack_error:
	if (need_alloc) {
		acct_gather_energy_destroy(energy_ptr);
		*energy = NULL;
	} else
		memset(energy_ptr, 0, sizeof(acct_gather_energy_t));

	return SLURM_ERROR;
}

extern int acct_gather_energy_g_update_node_energy(void)
{
	int retval = SLURM_ERROR;

	if (slurm_acct_gather_energy_init() < 0)
		return retval;

	retval = (*(ops.update_node_energy))();

	return retval;
}

extern int acct_gather_energy_g_get_data(enum acct_energy_type data_type,
					 void *data)
{
	int retval = SLURM_ERROR;

	if (slurm_acct_gather_energy_init() < 0)
		return retval;

	retval = (*(ops.get_data))(data_type, data);

	return retval;
}

extern int acct_gather_energy_g_set_data(enum acct_energy_type data_type,
					 void *data)
{
	int retval = SLURM_ERROR;

	if (slurm_acct_gather_energy_init() < 0)
		return retval;

	retval = (*(ops.set_data))(data_type, data);

	return retval;
}

extern int acct_gather_energy_startpoll(float frequency)
{
	int retval = SLURM_SUCCESS;
	pthread_attr_t attr;
	pthread_t _watch_node_thread_id;
	error("startpoll");

	if (slurm_acct_gather_energy_init() < 0)
		return SLURM_ERROR;

	if (!acct_shutdown) {
		error("acct_gather_energy_startpoll: "
		      "poll already started!");
		return retval;
	}

	acct_shutdown = false;

	freq = frequency;

	if (frequency == 0.0) {   /* don't want dynamic monitoring? */
		debug2("acct_gather_energy dynamic logging disabled");
		return retval;
	}

	/* create polling thread */
	slurm_attr_init(&attr);
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");

	if (pthread_create(&_watch_node_thread_id, &attr, &_watch_node, NULL)) {
		debug("acct_gather_energy failed to create _watch_node "
		      "thread: %m");
	} else
		debug3("acct_gather_energy dynamic logging enabled");
	slurm_attr_destroy(&attr);

	return retval;
}

extern void acct_gather_energy_g_conf_options(s_p_options_t **full_options,
					      int *full_options_cnt)
{
	if (slurm_acct_gather_energy_init() < 0)
		return;

	(*(ops.conf_options))(full_options, full_options_cnt);
}

extern void acct_gather_energy_g_conf_set(s_p_hashtbl_t *tbl)
{
	if (slurm_acct_gather_energy_init() < 0)
		return;

	(*(ops.conf_set))(tbl);
}

extern void acct_gather_energy_g_conf_values(void *data)
{
	if (slurm_acct_gather_energy_init() < 0)
		return;

	(*(ops.conf_values))(data);
}
