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
//#include <math.h>//Only for round() right now...

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
static unsigned int num_gpus = 0;
static uint64_t gpu_watts[MAX_GPUS];
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
}

static int _running_profile(void)
{
	return SLURM_SUCCESS;
}

static int _send_profile(void)
{return SLURM_SUCCESS;
}

extern int acct_gather_energy_p_update_node_energy(void)
{
	int rc = SLURM_SUCCESS;
	return rc;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	return SLURM_SUCCESS;
}

extern int fini(void)
{
	return SLURM_SUCCESS;
}

/* Need to make a copy of gpu_watts array, otherwise crashy crashy since this
 * copy is destroyed, freeing the array. */
void _copy_energy(acct_gather_energy_t *energy)
{
	/*error("Copy energy");int i;
	if (local_energy->gpu_watts) {
		for (i = 0; i < num_gpus; i++) {
			error("GPU%d: %dW", i, local_energy->gpu_watts[i]);
		}
	}
	else {
		error("gpu_watts not allocated!");
	}
	if (energy && energy->gpu_watts) {
		xfree(energy->gpu_watts);
	}*/


	/*if (num_gpus && local_energy->gpu_watts) {
		energy->gpu_watts = xmalloc(num_gpus * sizeof(uint64_t));
		if (!energy->gpu_watts) {
			return;
		}

		memcpy(energy->gpu_watts, local_energy->gpu_watts,
				num_gpus * sizeof(uint64_t));
	}*/
}

extern int acct_gather_energy_p_get_data(enum acct_energy_type data_type,
					 void *data) //TODO
{
	int rc = SLURM_SUCCESS;
	return rc;
}

extern int acct_gather_energy_p_set_data(enum acct_energy_type data_type,
					 void *data) // TODO
{
	int rc = SLURM_SUCCESS;
	return rc;
}

extern void acct_gather_energy_p_conf_options(s_p_options_t **full_options,
					      int *full_options_cnt) //TODO
{
	return;
}

extern void acct_gather_energy_p_conf_set(s_p_hashtbl_t *tbl) //TODO
{
	return;
}

extern void acct_gather_energy_p_conf_values(List *data) //TODO
{
	return;
}
