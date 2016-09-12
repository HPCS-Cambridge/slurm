/*****************************************************************************\
 *  task_cgroupv2.c - Library for task pre-launch and post_termination functions
 *		    for containment using linux cgroup subsystems
 *****************************************************************************
 *  Copyright (C) 2009 CEA/DAM/DIF
 *  Written by Matthieu Hautreux <matthieu.hautreux@cea.fr>
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

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#include <signal.h>
#include <sys/types.h>

#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"
#include "src/common/xcgroup_read_config.h"
#include "src/common/xstring.h"

#include "src/slurmd/slurmstepd/slurmstepd_job.h"

#include "src/slurmd/slurmd/slurmd.h"

#include "src/slurmd/common/xcgroup.h"

#include "task_cgroupv2.h"
#include "task_cgroupv2_memory.h"
#include "task_cgroupv2_blkio.h"

extern int task_p_add_pid (pid_t pid);
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
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "task" for task control) and <method> is a description
 * of how this plugin satisfies that application.  SLURM will only load
 * a task plugin if the plugin_type string has a prefix of "task/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "Tasks containment using linux cgroupv2";
const char plugin_type[]        = "task/cgroupv2";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

static bool use_memory  = false;
static bool use_blkio   = false;

static slurm_cgroup_conf_t slurm_cgroup_conf;

static char *slurm_cgpath = NULL;

static char user_cgroup_path[PATH_MAX];
static char job_cgroup_path[PATH_MAX];
static char jobstep_cgroup_path[PATH_MAX];

static xcgroup_t user_cg;
static xcgroup_t job_cg;
static xcgroup_t step_cg;

// based on blkio_cgroup_initialize
static int task_cgroupv2_initialize(xcgroup_ns_t *ns, xcgroup_t *cg, char *path, uid_t uid,
				gid_t gid, uint32_t notify)
{
	if(xcgroup_create(ns,cg,path,uid,gid) != XCGROUP_SUCCESS) {
		return SLURM_ERROR;
	}

	cg->notify = notify;

	if (xcgroup_instantiate(cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(cg);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

/* based on xcgroup_ns_is_available */
int cgroupv2_is_available(xcgroup_ns_t *ns)
{
	int fstatus;
	char *value;
	size_t s;
	xcgroup_t cg;

	if (xcgroup_create(ns, &cg, "/", 0, 0) == XCGROUP_ERROR) {
		return XCGROUP_ERROR;
	}

	/* release_agent doesn't exist anymore */
	/* This file should have contents (assuming all subsystems are available
	 * and enabled */
	if (xcgroup_get_param(&cg, "cgroup.controllers", &value, &s)
			!= XCGROUP_SUCCESS) {
		fstatus = XCGROUP_ERROR;
	}
	else {
		free(value);
		fstatus = XCGROUP_SUCCESS;
	}

	xcgroup_destroy(&cg);

	return fstatus;
}

/* based on xcgroup_ns_create */
int slurm_cgroupv2_init(slurm_cgroup_conf_t *slurm_cgroup_conf)
{
	root_ns.mnt_point = xstrdup(slurm_cgroup_conf->cgroup_mountpoint);
	root_ns.mnt_args = xstrdup("none");
	root_ns.subsystems = NULL;
	root_ns.notify_prog = NULL;//xstrdup_printf("%s/release",
		//conf->cgroup_release_agent);

	if(!cgroupv2_is_available) {
		if (slurm_cgroup_conf->cgroup_automount) {
			if(xcgroup_ns_mount(&root_ns)) {
				error("unable to mount root ns");
				xcgroup_ns_destroy(&root_ns);
				return XCGROUP_ERROR;
			}
		info("mounted root ns");
		}
		else {
			error("Root NS not mounted");
			xcgroup_ns_destroy(&root_ns);
			return XCGROUP_ERROR;
		}
	}

	if (XCGROUP_SUCCESS != xcgroup_create(&root_ns, &root_cg, "", 0, 0)) {
		error("task/cgroupv2: Cannot create root cgroup");
		return XCGROUP_ERROR;
	}
	
	if (NULL == (slurm_cgpath = task_cgroupv2_create_slurm_cg(&root_ns, &slurm_cg))) {
		error("task/cgroupv2: Cannot create slurm cgroup");
		return XCGROUP_ERROR;
	}
	else {
		error("task/cgroupv2: Created slurm cgroup");
		if (XCGROUP_SUCCESS != xcgroup_instantiate(&slurm_cg)) {
			xcgroup_destroy(&slurm_cg);
			error("task/cgroupv2: Could not instantiate slurm cgroup");
			return XCGROUP_ERROR;
		}
		else {
			error("task/cgroupv2: Instantiated slurm cgroup");
		}
	}
	

	/*
	 * Enable cgroupv2 subsystems in root and slurm cgroups. AFAIK, need to set
	 * these separately in every cgroup subdir
	 */
	if (slurm_cgroup_conf->constrain_ram_space ||
			slurm_cgroup_conf->constrain_swap_space) {
		use_memory = true;
		if (XCGROUP_SUCCESS != xcgroup_set_param(&root_cg, "cgroup.subtree_control", "+memory")) {
			error("Could not set root +memory");
			goto fail;
		}
		if (XCGROUP_SUCCESS != xcgroup_set_param(&slurm_cg, "cgroup.subtree_control", "+memory")) {
			error("Could not set slurm +memory");
			goto fail;
		}
	}

	if (true || slurm_cgroup_conf->io_quality_of_service) {
		use_blkio = true;
		if(XCGROUP_SUCCESS != xcgroup_set_param(&root_cg, "cgroup.subtree_control", "+io")) {
			error("Could not set root +io");
			goto fail;
		}
		if(XCGROUP_SUCCESS != xcgroup_set_param(&slurm_cg, "cgroup.subtree_control", "+io")) {
			error("Could not set slurm +io");
			goto fail;
		}
	}

	return XCGROUP_SUCCESS;

fail:
	//TODO
	return XCGROUP_ERROR;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 *	are called.  Put global initialization here.
 */
extern int init (void)
{
	error("LOADING CGROUPV2");

	/* read cgroup configuration */
	if (read_slurm_cgroup_conf(&slurm_cgroup_conf)) {
		return SLURM_ERROR;
	}

	/* 
	 * cgroupv2 has a unified hierarchy, so initialise the main SLURM cgroup
	 * independent from the selected subsystems
	 */
	if (XCGROUP_ERROR == slurm_cgroupv2_init(&slurm_cgroup_conf)) {
		return SLURM_ERROR;
	}

	if(false && use_memory) {
		if (task_cgroupv2_memory_init(&slurm_cgroup_conf) !=
		    SLURM_SUCCESS) {
			free_slurm_cgroup_conf(&slurm_cgroup_conf);
			return SLURM_ERROR;
		}
		debug("%s: now constraining jobs allocated memory",
		      plugin_type);
	}

	if(false && use_blkio) {
		if (task_cgroupv2_blkio_init(&slurm_cgroup_conf) !=
		    SLURM_SUCCESS) {
			free_slurm_cgroup_conf(&slurm_cgroup_conf);
			return SLURM_ERROR;
		}

		debug("%s: now enforcing I/O Quality of Service", plugin_type);
	}

	debug("%s: loaded", plugin_type);
	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is removed. Clear any allocated
 *	storage here.
 */
extern int fini (void)
{

	/*if (false && use_memory) {
		task_cgroupv2_memory_fini(&slurm_cgroup_conf);
	}
	if (false && use_blkio) {
		task_cgroupv2_blkio_fini(&slurm_cgroup_conf);
	}*/

	if (xcgroup_lock(&root_cg) == XCGROUP_SUCCESS) {
		xcgroup_delete(&slurm_cg);
		xcgroup_destroy(&slurm_cg);
		xcgroup_unlock(&root_cg);
		xcgroup_destroy(&root_cg);
		xcgroup_ns_destroy(&root_ns);
	}
	else {
		error("task/cgroupv2: unable to lock root cg");
	}

	/* unload configuration */
	free_slurm_cgroup_conf(&slurm_cgroup_conf);

	if (slurm_cgpath) {
		xfree(slurm_cgpath);
	}
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_batch_request()
 */
extern int task_p_slurmd_batch_request (uint32_t job_id,
					batch_job_launch_msg_t *req)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_launch_request()
 */
extern int task_p_slurmd_launch_request (uint32_t job_id,
					 launch_tasks_request_msg_t *req,
					 uint32_t node_id)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_reserve_resources()
 */
extern int task_p_slurmd_reserve_resources (uint32_t job_id,
					    launch_tasks_request_msg_t *req,
					    uint32_t node_id)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_suspend_job()
 */
extern int task_p_slurmd_suspend_job (uint32_t job_id)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_resume_job()
 */
extern int task_p_slurmd_resume_job (uint32_t job_id)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_slurmd_release_resources()
 */
extern int task_p_slurmd_release_resources (uint32_t job_id)
{
	return SLURM_SUCCESS;
}

/*
 * task_p_pre_setuid() is called before setting the UID for the
 * user to launch his jobs. Use this to create the CPUSET directory
 * and set the owner appropriately.
 */
extern int task_p_pre_setuid (stepd_step_rec_t *job)
{
	//if (use_memory) {
	//	/* we create the memory container as we are still root */
	//	task_cgroupv2_memory_create(job);
	//}

	//if (use_blkio) {
	//	task_cgroupv2_blkio_create(job);
	//}

	uint32_t jobid = job->jobid;
	uint32_t stepid = job->stepid;
	uid_t uid = job->uid;
	gid_t gid = job->gid;

	/* build user cgroup relative path if not set (should not be) */
	if (*user_cgroup_path == '\0') {
		if (snprintf(user_cgroup_path, PATH_MAX,
			     "%s/uid_%u", slurm_cgpath, uid) >= PATH_MAX) {
			error("unable to build uid %u cgroup relative "
			      "path : %m", uid);
			xfree(slurm_cgpath);
			return SLURM_ERROR;
		}
	}

	/* build job cgroup relative path if no set (should not be) */
	if (*job_cgroup_path == '\0') {
		if (snprintf(job_cgroup_path,PATH_MAX,"%s/job_%u",
			      user_cgroup_path,jobid) >= PATH_MAX) {
			error("task/cgroup: unable to build job %u blkio "
			      "cg relative path : %m", jobid);
			return SLURM_ERROR;
		}
	}

	/* build job step cgroup relative path (should not be) */
	if (*jobstep_cgroup_path == '\0') {
		int cc;
		if (stepid == SLURM_BATCH_SCRIPT) {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				      "%s/step_batch", job_cgroup_path);
		} else if (stepid == SLURM_EXTERN_CONT) {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				      "%s/step_extern", job_cgroup_path);
		} else {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				      "%s/step_%u",
				      job_cgroup_path, stepid);
		}
		if (cc >= PATH_MAX) {
			error("task/cgroup: unable to build job step %u.%u "
			      "blkio cg relative path : %m", jobid, stepid);
			return SLURM_ERROR;
		}
	}

	if (xcgroup_lock(&slurm_cg) != XCGROUP_SUCCESS) {
		error("task/cgroup: unable to lock slurm cg");
		return SLURM_ERROR;
	}

	/*
	 * Create user cgroup in the blkio ns (it could already exist)
	 * Ask for hierarchical blkio accounting starting from the user
	 * container in order to track the blkio consumption up to the
	 * user.
	 * We do not set any limits at this level for now. It could be
	 * interesting to do it in the future but blkio cgroup cleanup mech
	 * are not working well so it will be really difficult to manage
	 * addition/removal of blkio amounts at this level. (kernel 2.6.34)
	 */
	if (xcgroup_create(&root_ns, &user_cg,
			    user_cgroup_path,
			    getuid(),getgid()) != XCGROUP_SUCCESS) {
		goto error;
	}
	if (xcgroup_instantiate(&user_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&user_cg);
		goto error;
	}

	if (use_blkio) {
		xcgroup_set_param(&user_cg, "cgroup.subtree_control", "+io");
	}
	if (use_memory) {
		xcgroup_set_param(&user_cg, "cgroup.subtree_control", "+memory");
	}

	/*
	 * Create job cgroup in the cgroup ns (it could already exist)
	 * and set the associated cgroup limits.
	 * Disable notify_on_release for this blkio cgroup, it will be
	 * manually removed by the plugin at the end of the step.
	 */
	if (task_cgroupv2_initialize (&root_ns, &job_cg, job_cgroup_path,
	                      getuid(), getgid(), 0) < 0) {
		xcgroup_destroy (&user_cg);
		goto error;
	}

	if (use_blkio) {
		xcgroup_set_param(&job_cg, "cgroup.subtree_control", "+io");
	}
	if (use_memory) {
		xcgroup_set_param(&job_cg, "cgroup.subtree_control", "+memory");
	}


	/*
	 * Create step cgroup in the blkio ns (it should not exists)
	 * and set the associated blkio limits.
	 * Disable notify_on_release for the step blkio_cgroup, it will be
	 * manually removed by the plugin at the end of the step.
	 */
	if (task_cgroupv2_initialize (&root_ns, &step_cg, jobstep_cgroup_path,
	                      uid, gid, 0) < 0) {
		xcgroup_destroy(&user_cg);
		xcgroup_destroy(&job_cg);
		goto error;
	}

	xcgroup_unlock(&slurm_cg);
	return SLURM_SUCCESS;

error:
	xcgroup_unlock(&slurm_cg);

	return SLURM_ERROR;
}

/*
 * task_p_pre_launch_priv() is called prior to exec of application task.
 * in privileged mode, just after slurm_spank_task_init_privileged
 */
extern int task_p_pre_launch_priv (stepd_step_rec_t *job)
{
	task_p_add_pid(getpid());
	if (use_memory) {
		/* attach the task to the memory cgroup */
		task_cgroupv2_memory_attach_task(job);
	}

	if (use_blkio) {
		/* attach the task to the blkio cgroup */
		// TODO rename
		task_cgroupv2_blkio_prepare_step(job, &job_cg);
	}

	return SLURM_SUCCESS;
}

/*
 * task_p_pre_launch() is called prior to exec of application task.
 *	It is followed by TaskProlog program (from slurm.conf) and
 *	--task-prolog (from srun command line).
 */
extern int task_p_pre_launch (stepd_step_rec_t *job)
{
	return SLURM_SUCCESS;
}

/*
 * task_term() is called after termination of application task.
 *	It is preceded by --task-epilog (from srun command line)
 *	followed by TaskEpilog program (from slurm.conf).
 */
extern int task_p_post_term (stepd_step_rec_t *job, stepd_step_task_info_t *task)
{
	return SLURM_SUCCESS;//TODO
	static bool ran = false;

	/* Only run this on the first call since this will run for
	 * every task on the node.
	 */
	if (use_memory && !ran) {
		task_cgroupv2_memory_check_oom(job);
		ran = true;
	}
	return SLURM_SUCCESS;
}

/*
 * task_p_post_step() is called after termination of the step
 * (all the task)
 */
extern int task_p_post_step (stepd_step_rec_t *job)
{
	if (xcgroup_lock(&slurm_cg) == XCGROUP_SUCCESS) {
		xcgroup_delete(&step_cg);
		xcgroup_delete(&job_cg);
		xcgroup_delete(&user_cg);
		xcgroup_destroy(&step_cg);
		xcgroup_destroy(&job_cg);
		xcgroup_destroy(&user_cg);
		xcgroup_unlock(&slurm_cg);
	}
	else {
		error("task/cgroupv2: unable to lock slurm cg");
	}
	//fini();
	return SLURM_SUCCESS;
}

extern char* task_cgroupv2_create_slurm_cg (xcgroup_ns_t* ns, xcgroup_t *slurm_cg) {

	/* we do it here as we do not have access to the conf structure */
	/* in libslurm (src/common/xcgroup.c) */
	char* pre = (char*) xstrdup(slurm_cgroup_conf.cgroup_prepend);
#ifdef MULTIPLE_SLURMD
	if ( conf->node_name != NULL )
		xstrsubstitute(pre,"%n", conf->node_name);
	else {
		xfree(pre);
		pre = (char*) xstrdup("/slurm");
	}
#endif

	/* create slurm cgroup in the ns (it could already exist)
	 * disable notify_on_release to avoid the removal/creation
	 * of this cgroup for each last/first running job on the node */
	if (xcgroup_create(ns,slurm_cg,pre,
			   getuid(), getgid()) != XCGROUP_SUCCESS) {
		xfree(pre);
		return pre;
	}
	slurm_cg->notify = 0;
	if (xcgroup_instantiate(slurm_cg) != XCGROUP_SUCCESS) {
		error("unable to build slurm cgroup for ns %s: %m",
		      ns->subsystems);
		xcgroup_destroy(slurm_cg);
		xfree(pre);
		return pre;
	}
	else {
		debug3("slurm cgroup %s successfully created for ns %s: %m",
		       pre,ns->subsystems);
	}

	return pre;
}

/*
 * Add pid to specific cgroup.
 */
extern int task_p_add_pid (pid_t pid)
{
	//if (false && use_memory) {
	//	task_cgroupv2_memory_add_pid(pid);
	//}
	//if (use_blkio) {
	//	task_cgroupv2_blkio_add_pid(pid);
	//}

	xcgroup_add_pids_v2(&step_cg, &pid, 1);
	return SLURM_SUCCESS;
}