/***************************************************************************** \
 *  task_cgroup_blkio.c - blkio cgroup subsystem for task/cgroup
 *****************************************************************************
 *  Copyright (C) 2009 CEA/DAM/DIF
 *  Based on task_cgroup_memory.c by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *
 *  TODO personal (C)?
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

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "slurm/slurm_errno.h"
#include "slurm/slurm.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h" //?? TODO check if necessary
#include "src/slurmd/slurmd/slurmd.h" // Necessary

#include "task_cgroup.h"

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

static char user_cgroup_path[PATH_MAX];
static char job_cgroup_path[PATH_MAX];
static char jobstep_cgroup_path[PATH_MAX];

static xcgroup_ns_t blkio_ns;

static xcgroup_t user_blkio_cg;
static xcgroup_t job_blkio_cg;
static xcgroup_t step_blkio_cg;

extern int task_cgroup_blkio_init(slurm_cgroup_conf_t *slurm_cgroup_conf)
{
	user_cgroup_path[0] = '\0';
	job_cgroup_path[0] = '\0';
	jobstep_cgroup_path[0] = '\0';

	// TODO mount args?
	if (xcgroup_ns_create(slurm_cgroup_conf, &blkio_ns, "", "blkio")
	    != XCGROUP_SUCCESS) {
		error("task/cgroup: unable to create blkio namespace");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

extern int task_cgroup_blkio_fini(slurm_cgroup_conf_t *slurm_cgroup_conf)
{
	xcgroup_t blkio_cg;

	if (user_cgroup_path[0] == '\0' ||
	    job_cgroup_path[0] == '\0' ||
	    jobstep_cgroup_path[0] == '\0') {
		return SLURM_SUCCESS;
	}


	if (xcgroup_create(&blkio_ns,&blkio_cg,"",0,0) == XCGROUP_SUCCESS) {
		if (xcgroup_lock(&blkio_cg) == XCGROUP_SUCCESS) {
			if (xcgroup_delete(&step_blkio_cg) != SLURM_SUCCESS)
				debug2("task/cgroup: unable to remove step "
				       "blkio cgroup : %m");
			if (xcgroup_delete(&job_blkio_cg) != XCGROUP_SUCCESS)
				debug2("task/cgroup: not removing "
				       "job blkio cgroup : %m");
			if (xcgroup_delete(&user_blkio_cg) != XCGROUP_SUCCESS)
				debug2("task/cgroup: not removing "
				       "user blkio cgroup : %m");
			xcgroup_unlock(&blkio_cg);
		} else
			error("task/cgroup: unable to lock root blkio cgroup : %m");
		xcgroup_destroy(&blkio_cg);
	} else
		error("task/cgroup: unable to create root blkio cgroup : %m");

	xcgroup_destroy(&user_blkio_cg);
	xcgroup_destroy(&job_blkio_cg);
	xcgroup_destroy(&step_blkio_cg);

	user_cgroup_path[0]='\0';
	job_cgroup_path[0]='\0';
	jobstep_cgroup_path[0]='\0';

	xcgroup_ns_destroy(&blkio_ns);

	return SLURM_SUCCESS;
}

static int blkio_cgroup_initialize (xcgroup_ns_t *ns, xcgroup_t *cg, char *path, uid_t uid,
			     gid_t gid, uint32_t notify)
{

	if (xcgroup_create (ns, cg, path, uid, gid) != XCGROUP_SUCCESS)
		return SLURM_ERROR;

	cg->notify = notify;

	if (xcgroup_instantiate (cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy (cg);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

extern int task_cgroup_blkio_create(stepd_step_rec_t *job)
{
	int fstatus = SLURM_ERROR;
	xcgroup_t blkio_cg;
	uint32_t jobid = job->jobid;
	uint32_t stepid = job->stepid;
	uid_t uid = job->uid;
	gid_t gid = job->gid;
	char *slurm_cgpath;

	/* create slurm root cg in this cg namespace */
	slurm_cgpath = task_cgroup_create_slurm_cg(&blkio_ns);
	if ( slurm_cgpath == NULL ) {
		return SLURM_ERROR;
	}

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
	xfree(slurm_cgpath);

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

	/*
	 * create blkioory root cg and lock it
	 *
	 * we will keep the lock until the end to avoid the effect of a release
	 * agent that would remove an existing cgroup hierarchy while we are
	 * setting it up. As soon as the step cgroup is created, we can release
	 * the lock.
	 * Indeed, consecutive slurm steps could result in cg being removed
	 * between the next EEXIST instanciation and the first addition of
	 * a task. The release_agent will have to lock the root blkio cgroup
	 * to avoid this scenario.
	 */
	if (xcgroup_create(&blkio_ns, &blkio_cg, "",0,0) != XCGROUP_SUCCESS) {
		error("task/cgroup: unable to create root blkio xcgroup");
		return SLURM_ERROR;
	}
	if (xcgroup_lock(&blkio_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&blkio_cg);
		error("task/cgroup: unable to lock root blkio cg");
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
	if (xcgroup_create(&blkio_ns, &user_blkio_cg,
			    user_cgroup_path,
			    getuid(),getgid()) != XCGROUP_SUCCESS) {
		goto error;
	}
	if (xcgroup_instantiate(&user_blkio_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&user_blkio_cg);
		goto error;
	}

	/*
	 * Create job cgroup in the cgroup ns (it could already exist)
	 * and set the associated cgroup limits.
	 * Disable notify_on_release for this blkio cgroup, it will be
	 * manually removed by the plugin at the end of the step.
	 */
	if (blkio_cgroup_initialize (&blkio_ns, &job_blkio_cg, job_cgroup_path,
	                      getuid(), getgid(), 0) < 0) {
		xcgroup_destroy (&user_blkio_cg);
		goto error;
	}

	/*
	 * Create step cgroup in the blkio ns (it should not exists)
	 * and set the associated blkio limits.
	 * Disable notify_on_release for the step blkio_cgroup, it will be
	 * manually removed by the plugin at the end of the step.
	 */
	if (blkio_cgroup_initialize (&blkio_ns, &step_blkio_cg, jobstep_cgroup_path,
	                      uid, gid, 0) < 0) {
		xcgroup_destroy(&user_blkio_cg);
		xcgroup_destroy(&job_blkio_cg);
		goto error;
	}

error:
	xcgroup_unlock(&blkio_cg);
	xcgroup_destroy(&blkio_cg);

	return fstatus;
}

extern int task_cgroup_blkio_attach_task(stepd_step_rec_t *job)
{
	int fstatus = SLURM_ERROR;
	pid_t pid;

	/* Set IO QoS (right now, all-devices blkio.weight value) */
	/* len(UINT16_T_MAX) + \0 = 6 chars .*/
	char io_qos[6];
	sprintf(io_qos, "%"PRIu16"", job->io_qos);
	xcgroup_set_param(&step_blkio_cg, "blkio.weight", io_qos);

	/*
	 * Attach the current task to the step blkio cgroup
	 */
	pid = getpid();
	if (xcgroup_add_pids(&step_blkio_cg, &pid, 1) != XCGROUP_SUCCESS) {
		error("task/cgroup: unable to add task[pid=%u] to "
		      "blkio cg '%s'",pid,step_blkio_cg.path);
		fstatus = SLURM_ERROR;
	} else
		fstatus = SLURM_SUCCESS;

	return fstatus;
}

extern int task_cgroup_blkio_add_pid(pid_t pid)
{
	return xcgroup_add_pids(&step_blkio_cg, &pid, 1);
}
