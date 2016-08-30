/*****************************************************************************\
 *  task_cgroupv2_blkio.h - memory cgroupv2 subsystem primitives for task/cgroupv2
 *****************************************************************************
 *  Copyright (C) 2009 CEA/DAM/DIF
 *  Based on task_cgroupv2_memory.h by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *
 *  TODO own (C)?
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
#   include "config.h"
#endif

#ifndef _TASK_CGROUP_BLKIOV2_H_
#define _TASK_CGROUP_BLKIOV2_H_

#include "src/common/xcgroup_read_config.h"

/* initialize memory subsystem of task/cgroupv2 */
extern int task_cgroupv2_blkio_init(slurm_cgroup_conf_t *slurm_cgroup_conf);

/* release memory subsystem resources */
extern int task_cgroupv2_blkio_fini(slurm_cgroup_conf_t *slurm_cgroup_conf);

/* create user/job/jobstep memory cgroupv2s */
extern int task_cgroupv2_blkio_create(stepd_step_rec_t *job);

/* create a task cgroupv2 and attach the task to it */
extern int task_cgroupv2_blkio_prepare_step(stepd_step_rec_t *job, xcgroup_t *step_cg);

/* detect if oom ran on a step or job and print notice of said event */
extern int task_cgroupv2_blkio_check_oom(stepd_step_rec_t *job);

/* add a pid to the cgroupv2 */
extern int task_cgroupv2_blkio_add_pid(pid_t pid);

#endif
