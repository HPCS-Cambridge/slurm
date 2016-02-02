/*****************************************************************************\
 *  acct_gather_energy_nvml.c - slurm energy accounting plugin for NVML.
 *****************************************************************************
 *  Copyright (C) 2012
 *  Written by Bull- Yiannis Georgiou // TODO modify
 *  CODE-OCEC-09-009. All rights reserved.
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
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
\*****************************************************************************/

/*   acct_gather_energy_nvml
 * This plugin does not initiate a node-level thread.
 * Performs Nvidia GPU energy profiling based on NVML.
 * Based off the RAPL plugin by Bull/Yiannis Georgiou, TODO fix credits etc.
 *
 * NOTE Will report GPU energy as CPU energy at first, will change later TODO.
 */


#include <fcntl.h>
#include <signal.h>
#include "src/common/slurm_xlator.h"
#include "src/common/slurm_acct_gather_energy.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/fd.h"
#include "src/slurmd/common/proctrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>//Only for round() right now...

#include <nvidia/gdk/nvml.h>

#define _DEBUG 1
#define _DEBUG_ENERGY 1

#define MAX_GPUS 8

#define NVCHECK(_err) {								\
	if(NVML_SUCCESS != _err) {					\
		error("nvml: %s", __FUNCTION__);	\
		return SLURM_ERROR;								\
	}																		\
}

#define NVCHECKV(_err) {							\
	if(NVML_SUCCESS != _err) {					\
		error("nvml: %s", __FUNC__);			\
		return;														\
	}																		\
}

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for SLURM job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "AcctGatherEnergy NVML plugin";
const char plugin_type[] = "acct_gather_energy/nvml";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;


static uint64_t debug_flags = 0;

static int dataset_id = -1; /* id of the dataset for profile data */

// END vars_from_rapl
static nvmlReturn_t nverr = NVML_SUCCESS;
static unsigned int num_gpus = 0;
static nvmlDevice_t *gpus = NULL;
static int *gpu_watts;
static acct_gather_energy_t *local_energy = NULL;

static bool _run_in_daemon(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = 1;
		run = run_in_daemon("slurmd,slurmstepd");
	}

	return run;
}

static void _get_joules_task(acct_gather_energy_t *energy) // One of our main buddies
{
	int i;
	int power, power_sum = 0;

	xassert(_run_in_daemon());

	if (!energy->gpu_watts) {
		energy->gpu_watts = xmalloc(num_gpus * sizeof(uint64_t));
		if (!energy->gpu_watts) {
			error("Could not allocate gpu_watts");
			return;
		}
	}

	for (i = 0; i < num_gpus; i++) {
		/*NVCHECK(nvmlDeviceGetPowerUsage(gpus[i], &power));*/
		//error("[%d] Errval: %d", i, nvmlDeviceGetPowerUsage(gpus[i], &power));
		power = rand(); // TODO necessary until Fermi GPU is available.
		power = round(power/1000.);
		power_sum += power;
		energy->gpu_watts[i] = (uint64_t)power;
	}

	//power_sum = round(power_sum/1000.); //TODO  Do we want to round? Floor? Or just keep milliwatts? Or both (round/floor report, milliwatts for further calculations)? This line is the reason we need -lm by the way

	//error("[GPU]Total power: %dW", power_sum);

	if (energy->consumed_energy) {
		uint16_t node_freq;
		energy->consumed_energy =
			(uint64_t)power_sum - energy->base_consumed_energy;
		energy->current_watts = (uint32_t)power_sum;
		node_freq = slurm_get_acct_gather_node_freq();
		//if (node_freq)	/* Prevent divide by zero */
			//energy->current_watts /= (float)node_freq;
	} else {
		energy->consumed_energy = 1;
		energy->base_consumed_energy = (uint64_t)power_sum;
	}
	energy->previous_consumed_energy = (uint64_t)power_sum;
	energy->poll_time = time(NULL);

	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("_get_joules_task: current %u Watts, "
		     "consumed %"PRIu64"",
		     power_sum, energy->consumed_energy);
}

static int _running_profile(void)
{
	static bool run = false;
	static uint32_t profile_opt = ACCT_GATHER_PROFILE_NOT_SET;

	if (profile_opt == ACCT_GATHER_PROFILE_NOT_SET) {
		acct_gather_profile_g_get(ACCT_GATHER_PROFILE_RUNNING,
					  &profile_opt);
		if (profile_opt & ACCT_GATHER_PROFILE_ENERGY)
			run = true;
	}

	return run;
}

static int _send_profile(void)
{
	int i;
	char name[7];
	uint64_t curr_watts;
	acct_gather_profile_dataset_t *dataset = NULL;

	if (!_running_profile())
		return SLURM_SUCCESS;

	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("_send_profile: consumed %u watts",
		     local_energy->current_watts);

	if (dataset_id < 0) {
		/* Create data set template first */
		if (!num_gpus) {
			error("Energy: No GPUs identified");
			return SLURM_ERROR;
		}
		dataset = xmalloc((num_gpus+1) * sizeof(acct_gather_profile_dataset_t));
		if (!dataset) {
			error("Energy: Could not allocate memory for dataset");
			return SLURM_ERROR;
		}
		for (i = 0; i < num_gpus; i++) {
			sprintf(name, "GPU%d", i);
			dataset[i].name = xstrdup(name);
			dataset[i].type = PROFILE_FIELD_UINT64;
		}
		dataset[num_gpus].name = NULL;
		dataset[num_gpus].type = PROFILE_FIELD_NOT_SET;

		/* Now create data set */
		dataset_id = acct_gather_profile_g_create_dataset(
			"Energy", NO_PARENT, dataset);

		for (i = 0; i < num_gpus; i++) {
			xfree(dataset[i].name);
		}
		xfree(dataset); // Done with this

		if (debug_flags & DEBUG_FLAG_ENERGY)
			debug("Energy: dataset created (id = %d)", dataset_id);
		if (dataset_id == SLURM_ERROR) {
			error("Energy: Failed to create the dataset for NVML");
			return SLURM_ERROR;
		}
	}

	//curr_watts = (uint64_t)local_energy->current_watts; error("Current watts: %lu", curr_watts);
	if (debug_flags & DEBUG_FLAG_PROFILE) {
		info("PROFILE-Energy: power=%u", local_energy->current_watts);
	}

	return acct_gather_profile_g_add_sample_data(dataset_id,
	                                             (void *)local_energy->gpu_watts,
						     local_energy->poll_time);
}

extern int acct_gather_energy_p_update_node_energy(void)
{
	int rc = SLURM_SUCCESS;
	//error("update node energy");

	xassert(_run_in_daemon());

	if (local_energy->current_watts == NO_VAL) {
		error("update_node_energy, local energy, no val");
	}

	if (local_energy->current_watts == NO_VAL)
		return rc;

	_get_joules_task(local_energy);

	return rc;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	int i;
	debug_flags = slurm_get_debug_flags();

	if(!_run_in_daemon()) {
		return SLURM_SUCCESS;
	}

	nverr = nvmlInit();
	NVCHECK(nverr);
	debug("NVML initialised");
	NVCHECK(nvmlDeviceGetCount(&num_gpus));
	debug("Found %d GPUs", num_gpus);

	if (num_gpus > MAX_GPUS) {
		error("Too many GPUS!");
		return SLURM_ERROR;
	}

	gpus = xmalloc(num_gpus*sizeof(nvmlDevice_t));
	if (!gpus) {
		error("Energy: init: Could not allocate memory for GPU devices");
		return SLURM_ERROR;
	}

	for (i = 0; i < num_gpus; i++) {
		NVCHECK(nvmlDeviceGetHandleByIndex(i, &gpus[i]));
	}num_gpus=8;

	gpu_watts = xmalloc(num_gpus * sizeof(int));
	if (!gpu_watts) {
		error("Energy: init: Could not allocate memory for GPU power");
		return SLURM_ERROR;
	}

	local_energy = acct_gather_energy_alloc(1); // WHY is there no retval checking in this func?
	local_energy->num_gpus = num_gpus;

	/* put anything that requires the .conf being read in
	   acct_gather_energy_p_conf_parse
	*/

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	if (!_run_in_daemon())
		return SLURM_SUCCESS;

	nverr = nvmlShutdown();
	NVCHECK(nverr);
	debug("NVML Terminated");

	acct_gather_energy_destroy(local_energy);
	local_energy = NULL;
	xfree(gpus); // DO NOT free contained pointers, that'll break. Hard.
	gpus = NULL;
	return SLURM_SUCCESS;
}

/* Need to make a copy of gpu_watts array, otherwise crashy crashy since this
 * copy is destroyed, freeing the array. */
void _copy_energy(acct_gather_energy_t *energy)
{
	if (energy && energy->gpu_watts) {
		xfree(energy->gpu_watts);
	}

	memcpy(energy, local_energy, sizeof(acct_gather_energy_t));

	if (num_gpus && local_energy->gpu_watts) {
		energy->gpu_watts = xmalloc(num_gpus * sizeof(uint64_t));
		if (!energy->gpu_watts) {
			return;
		}

		memcpy(energy->gpu_watts, local_energy->gpu_watts,
				num_gpus * sizeof(uint64_t));
	}
}

extern int acct_gather_energy_p_get_data(enum acct_energy_type data_type,
					 void *data) //TODO
{
	int rc = SLURM_SUCCESS;
	acct_gather_energy_t *energy = (acct_gather_energy_t *)data;
	time_t *last_poll = (time_t *)data;
	uint16_t *sensor_cnt = (uint16_t *)data;

	xassert(_run_in_daemon());
	//error("[NVML] get_data: dt = %d", data_type);

	switch (data_type) {
	case ENERGY_DATA_JOULES_TASK:
	case ENERGY_DATA_NODE_ENERGY_UP:
		if (local_energy->current_watts == NO_VAL)
			energy->consumed_energy = NO_VAL;
		else
			_get_joules_task(energy);
		break;
	case ENERGY_DATA_STRUCT:
	case ENERGY_DATA_NODE_ENERGY:
		if (energy == local_energy) { // Probably unnecessary;
			break;
		}
		_copy_energy(energy);
		break;
	case ENERGY_DATA_LAST_POLL:
		*last_poll = local_energy->poll_time;
		break;
	case ENERGY_DATA_SENSOR_CNT:
		*sensor_cnt = num_gpus; // This right? originally just '1' (int not char).
		error("TODO: WHAT SHOULD WE DO WITH ENERGY_DATA_SENSORS_CNT?"); //TODO
		break;
	default:
		error("acct_gather_energy_p_get_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern int acct_gather_energy_p_set_data(enum acct_energy_type data_type,
					 void *data) // TODO
{
	int rc = SLURM_SUCCESS;

	xassert(_run_in_daemon());

	//error("[NVML] set_data");

	switch (data_type) {
	case ENERGY_DATA_RECONFIG:
		debug_flags = slurm_get_debug_flags();
		break;
	case ENERGY_DATA_PROFILE:
		_get_joules_task(local_energy);
		_send_profile();
		break;
	default:
		error("acct_gather_energy_p_set_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern void acct_gather_energy_p_conf_options(s_p_options_t **full_options,
					      int *full_options_cnt) //TODO
{
	return;
}

extern void acct_gather_energy_p_conf_set(s_p_hashtbl_t *tbl) //TODO
{

	if (!_run_in_daemon())
		return;

	debug("%s loaded", plugin_name);

	return;
}

extern void acct_gather_energy_p_conf_values(List *data) //TODO
{
	return;
}
