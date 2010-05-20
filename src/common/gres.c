/*****************************************************************************\
 *  gres.c - driver for gres plugin
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
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
#  include "config.h"
#  if STDC_HEADERS
#    include <string.h>
#  endif
#  if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#  endif /* HAVE_SYS_TYPES_H */
#  if HAVE_UNISTD_H
#    include <unistd.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  else /* ! HAVE_INTTYPES_H */
#    if HAVE_STDINT_H
#      include <stdint.h>
#    endif
#  endif /* HAVE_INTTYPES_H */
#else /* ! HAVE_CONFIG_H */
#  include <sys/types.h>
#  include <unistd.h>
#  include <stdint.h>
#  include <string.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>

#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define GRES_MAGIC 0x438a34d4

typedef struct slurm_gres_ops {
	uint32_t	(*plugin_id);
	int		(*help_msg)		( char *msg, int msg_size );
	int		(*node_config_init)	( char *node_name,
						  char *orig_config,
						  void **gres_data );
	int		(*node_config_load)	( void );
	int		(*node_config_pack)	( Buf buffer );
	int		(*node_config_unpack)	( Buf buffer );
	int		(*node_config_validate)	( char *node_name,
						  char *orig_config,
						  char **new_config,
						  void **gres_data,
						  uint16_t fast_schedule,
						  char **reason_down );
	void		(*node_config_delete)	( void *gres_data );
	int		(*node_reconfig)	( char *node_name,
						  char *orig_config,
						  char **new_config,
						  void **gres_data,
						  uint16_t fast_schedule );
	int		(*node_state_pack)	( void *gres_data,
						  Buf buffer );
	int		(*node_state_unpack)	( void **gres_data,
						  Buf buffer );
	void *		(*node_state_dup)	( void *gres_data );
	void		(*node_state_dealloc)	( void *gres_data );
	int		(*node_state_realloc)	( void *job_gres_data,
						  int node_offset,
						  void *node_gres_data );
	void		(*node_state_log)	( void *gres_data,
						  char *node_name );

	void		(*job_state_delete)	( void *gres_data );
	int		(*job_state_validate)	( char *config,
						  void **gres_data);
	void *		(*job_state_dup)	( void *gres_data );
	int		(*job_state_pack)	( void *gres_data,
						  Buf buffer );
	int		(*job_state_unpack)	( void **gres_data,
						  Buf buffer );
	void		(*job_state_log)	( void *gres_data,
						  uint32_t job_id );
	uint32_t	(*job_test)		( void *job_gres_data,
						  void *node_gres_data,
						  bool use_total_gres );
	int		(*job_alloc)		( void *job_gres_data,
						  void *node_gres_data,
						  int node_cnt,
						  int node_offset,
						  uint32_t cpu_cnt );
	int		(*job_dealloc)		( void *job_gres_data,
						  void *node_gres_data,
						  int node_offset );
} slurm_gres_ops_t;

typedef struct slurm_gres_context {
	char	       	*gres_type;
	plugrack_t     	plugin_list;
	plugin_handle_t	cur_plugin;
	int		gres_errno;
	slurm_gres_ops_t ops;
	bool		unpacked_info;
} slurm_gres_context_t;

static int gres_context_cnt = -1;
static bool gres_debug = false;
static slurm_gres_context_t *gres_context = NULL;
static char *gres_plugin_list = NULL;
static pthread_mutex_t gres_context_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct gres_state {
	uint32_t	plugin_id;
	void		*gres_data;
} gres_state_t;

/* Variant of strcmp that will accept NULL string pointers */
static int  _strcmp(const char *s1, const char *s2)
{
	if ((s1 != NULL) && (s2 == NULL))
		return 1;
	if ((s1 == NULL) && (s2 == NULL))
		return 0;
	if ((s1 == NULL) && (s2 != NULL))
		return -1;
	return strcmp(s1, s2);
}

static int _load_gres_plugin(char *plugin_name,
			     slurm_gres_context_t *plugin_context)
{
	/*
	 * Must be synchronized with slurm_gres_ops_t above.
	 */
	static const char *syms[] = {
		"plugin_id",
		"help_msg",
		"node_config_init",
		"node_config_load",
		"node_config_pack",
		"node_config_unpack",
		"node_config_validate",
		"node_config_delete",
		"node_reconfig",
		"node_state_pack",
		"node_state_unpack",
		"node_state_dup",
		"node_state_dealloc",
		"node_state_realloc",
		"node_state_log",
		"job_state_delete",
		"job_state_validate",
		"job_state_dup",
		"job_state_pack",
		"job_state_unpack",
		"job_state_log",
		"job_test",
		"job_alloc",
		"job_dealloc"
	};
	int n_syms = sizeof(syms) / sizeof(char *);

	/* Find the correct plugin */
	plugin_context->gres_type	= xstrdup("gres/");
	xstrcat(plugin_context->gres_type, plugin_name);
	plugin_context->plugin_list	= NULL;
	plugin_context->cur_plugin	= PLUGIN_INVALID_HANDLE;
	plugin_context->gres_errno 	= SLURM_SUCCESS;

	plugin_context->cur_plugin = plugin_load_and_link(
					plugin_context->gres_type,
					n_syms, syms,
					(void **) &plugin_context->ops);
	if (plugin_context->cur_plugin != PLUGIN_INVALID_HANDLE)
		return SLURM_SUCCESS;

	error("gres: Couldn't find the specified plugin name for %s "
	      "looking at all files",
	      plugin_context->gres_type);

	/* Get plugin list */
	if (plugin_context->plugin_list == NULL) {
		char *plugin_dir;
		plugin_context->plugin_list = plugrack_create();
		if (plugin_context->plugin_list == NULL) {
			error("gres: cannot create plugin manager");
			return SLURM_ERROR;
		}
		plugrack_set_major_type(plugin_context->plugin_list,
					"gres");
		plugrack_set_paranoia(plugin_context->plugin_list,
				      PLUGRACK_PARANOIA_NONE, 0);
		plugin_dir = slurm_get_plugin_dir();
		plugrack_read_dir(plugin_context->plugin_list, plugin_dir);
		xfree(plugin_dir);
	}

	plugin_context->cur_plugin = plugrack_use_by_type(
					plugin_context->plugin_list,
					plugin_context->gres_type );
	if (plugin_context->cur_plugin == PLUGIN_INVALID_HANDLE) {
		error("gres: cannot find scheduler plugin for %s",
		       plugin_context->gres_type);
		return SLURM_ERROR;
	}

	/* Dereference the API. */
	if (plugin_get_syms(plugin_context->cur_plugin,
			    n_syms, syms,
			    (void **) &plugin_context->ops ) < n_syms ) {
		error("gres: incomplete %s plugin detected",
		      plugin_context->gres_type);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static int _unload_gres_plugin(slurm_gres_context_t *plugin_context)
{
	int rc;

	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if (plugin_context->plugin_list)
		rc = plugrack_destroy(plugin_context->plugin_list);
	else {
		rc = SLURM_SUCCESS;
		plugin_unload(plugin_context->cur_plugin);
	}
	xfree(plugin_context->gres_type);

	return rc;
}

/*
 * Initialize the gres plugin.
 *
 * Returns a SLURM errno.
 */
extern int gres_plugin_init(void)
{
	int i, j, rc = SLURM_SUCCESS;
	char *last = NULL, *names, *one_name, *full_name;

	slurm_mutex_lock(&gres_context_lock);
	if (slurm_get_debug_flags() & DEBUG_FLAG_GRES)
		gres_debug = true;
	else
		gres_debug = false;

	if (gres_context_cnt >= 0)
		goto fini;

	gres_plugin_list = slurm_get_gres_plugins();
	gres_context_cnt = 0;
	if ((gres_plugin_list == NULL) || (gres_plugin_list[0] == '\0'))
		goto fini;

	gres_context_cnt = 0;
	names = xstrdup(gres_plugin_list);
	one_name = strtok_r(names, ",", &last);
	while (one_name) {
		full_name = xstrdup("gres/");
		xstrcat(full_name, one_name);
		for (i=0; i<gres_context_cnt; i++) {
			if (!strcmp(full_name, gres_context[i].gres_type))
				break;
		}
		xfree(full_name);
		if (i<gres_context_cnt) {
			error("Duplicate plugin %s ignored",
			      gres_context[i].gres_type);
		} else {
			xrealloc(gres_context, (sizeof(slurm_gres_context_t) *
				 (gres_context_cnt + 1)));
			rc = _load_gres_plugin(one_name,
					       gres_context + gres_context_cnt);
			if (rc != SLURM_SUCCESS)
				break;
			gres_context_cnt++;
		}
		one_name = strtok_r(NULL, ",", &last);
	}
	xfree(names);

	/* Insure that plugin_id is valid and unique */
	for (i=0; i<gres_context_cnt; i++) {
		for (j=i+1; j<gres_context_cnt; j++) {
			if (*(gres_context[i].ops.plugin_id) !=
			    *(gres_context[j].ops.plugin_id))
				continue;
			fatal("GresPlugins: Duplicate plugin_id %u for %s and %s",
			      *(gres_context[i].ops.plugin_id),
			      gres_context[i].gres_type,
			      gres_context[j].gres_type);
		}
		if (*(gres_context[i].ops.plugin_id) < 100) {
			fatal("GresPlugins: Invalid plugin_id %u (<100) %s",
			      *(gres_context[i].ops.plugin_id),
			      gres_context[i].gres_type);
		}

	}

fini:	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

/*
 * Terminate the gres plugin. Free memory.
 *
 * Returns a SLURM errno.
 */
extern int gres_plugin_fini(void)
{
	int i, j, rc = SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
	if (gres_context_cnt < 0)
		goto fini;

	for (i=0; i<gres_context_cnt; i++) {
		j = _unload_gres_plugin(gres_context + i);
		if (j != SLURM_SUCCESS)
			rc = j;
	}
	xfree(gres_context);
	xfree(gres_plugin_list);
	gres_context_cnt = -1;

fini:	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

/*
 **************************************************************************
 *                          P L U G I N   C A L L S                       *
 **************************************************************************
 */

/*
 * Provide a plugin-specific help message for salloc, sbatch and srun
 * IN/OUT msg - buffer provided by caller and filled in by plugin
 * IN msg_size - size of msg buffer in bytes
 */
extern int gres_plugin_help_msg(char *msg, int msg_size)
{
	int i, rc;
	char *tmp_msg;

	if (msg_size < 1)
		return EINVAL;

	msg[0] = '\0';
	tmp_msg = xmalloc(msg_size);
	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(gres_context[i].ops.help_msg))(tmp_msg, msg_size);
		if ((rc != SLURM_SUCCESS) || (tmp_msg[0] == '\0'))
			continue;
		if ((strlen(msg) + strlen(tmp_msg) + 2) > msg_size)
			break;
		if (msg[0])
			strcat(msg, "\n");
		strcat(msg, tmp_msg);
		tmp_msg[0] = '\0';
	}
	slurm_mutex_unlock(&gres_context_lock);

	xfree(tmp_msg);
	return rc;
}

/*
 * Perform reconfig, re-read any configuration files
 * OUT did_change - set if gres configuration changed
 */
extern int gres_plugin_reconfig(bool *did_change)
{
	int rc = SLURM_SUCCESS;
	char *plugin_names = slurm_get_gres_plugins();
	bool plugin_change;

	*did_change = false;
	slurm_mutex_lock(&gres_context_lock);
	if (slurm_get_debug_flags() & DEBUG_FLAG_GRES)
		gres_debug = true;
	else
		gres_debug = false;

	if (_strcmp(plugin_names, gres_plugin_list))
		plugin_change = true;
	else
		plugin_change = false;
	slurm_mutex_unlock(&gres_context_lock);

	if (plugin_change) {
		error("GresPlugins changed from %s to %s ignored",
		     gres_plugin_list, plugin_names);
		error("Restart the slurmctld daemon to change GresPlugins");
		*did_change = true;
#if 0
		/* This logic would load new plugins, but we need the old
		 * plugins to persist in order to process old state 
		 * information. */
		rc = gres_plugin_fini();
		if (rc == SLURM_SUCCESS)
			rc = gres_plugin_init();
#endif
	}
	xfree(plugin_names);

	return rc;
}

/*
 * Load this node's gres configuration (i.e. how many resources it has)
 */
extern int gres_plugin_node_config_load(void)
{
	int i, rc;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(gres_context[i].ops.node_config_load))();
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Pack this node's gres configuration into a buffer
 *
 * Data format:
 *	uint32_t	magic cookie
 *	uint32_t	plugin_id name (must be unique)
 *	uint32_t	gres data block size
 *	void *		gres data packed by plugin
 */
extern int gres_plugin_node_config_pack(Buf buffer)
{
	int i, rc;
	uint32_t gres_size = 0, magic = GRES_MAGIC;
	uint32_t header_offset, data_offset, tail_offset;
	uint16_t rec_cnt;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	rec_cnt = MAX(0, gres_context_cnt);
	pack16(rec_cnt, buffer);
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		pack32(magic, buffer);
		pack32(*(gres_context[i].ops.plugin_id), buffer);
		header_offset = get_buf_offset(buffer);
		pack32(gres_size, buffer);	/* Placeholder for now */
		data_offset = get_buf_offset(buffer);
		rc = (*(gres_context[i].ops.node_config_pack))(buffer);
		tail_offset = get_buf_offset(buffer);
		gres_size = tail_offset - data_offset;
		set_buf_offset(buffer, header_offset);
		pack32(gres_size, buffer);	/* The actual buffer size */
		set_buf_offset(buffer, tail_offset);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Unpack this node's configuration from a buffer
 * IN buffer - message buffer to unpack
 * IN node_name - name of node whose data is being unpacked
 */
extern int gres_plugin_node_config_unpack(Buf buffer, char* node_name)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;

	rc = gres_plugin_init();
	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while (rc == SLURM_SUCCESS) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_node_config_unpack: no plugin "
			      "configured to unpack data type %u from node %s",
			      plugin_id, node_name);
			/* A likely sign that GresPlugins is inconsistently
			 * configured. Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc = (*(gres_context[i].ops.node_config_unpack))(buffer);
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the node. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		error("gres_plugin_node_config_unpack: no info packed for %s "
		      "by node %s",
		      gres_context[i].gres_type, node_name);
		rc2 = (*(gres_context[i].ops.node_config_unpack))(NULL);
		if (rc2 != SLURM_SUCCESS)
			rc = rc2;
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_node_config_unpack: unpack error from node %s",
	      node_name);
	rc = SLURM_ERROR;
	goto fini;
}

static void _gres_node_list_delete(void *list_element)
{
	int i;
	gres_state_t *gres_ptr;

	if (gres_plugin_init() != SLURM_SUCCESS)
		return;

	gres_ptr = (gres_state_t *) list_element;
	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_ptr->plugin_id != *(gres_context[i].ops.plugin_id))
			continue;
		(*(gres_context[i].ops.node_config_delete))
				(gres_ptr->gres_data);
		xfree(gres_ptr);
		break;
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Build a node's gres record based only upon the slurm.conf contents
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT gres_list - List of Gres records for this node to track usage
 */
extern int gres_plugin_init_node_config(char *node_name, char *orig_config,
					List *gres_list)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find or create gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			list_append(*gres_list, gres_ptr);
		}

		rc = (*(gres_context[i].ops.node_config_init))
			(node_name, orig_config, &gres_ptr->gres_data);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Validate a node's configuration and put a gres record onto a list
 * Called immediately after gres_plugin_node_config_unpack().
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT new_config - Updated gres info from slurm.conf if FastSchedule=0
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN fast_schedule - 0: Validate and use actual hardware configuration
 *		      1: Validate hardware config, but use slurm.conf config
 *		      2: Don't validate hardware, use slurm.conf configuration
 * OUT reason_down - set to an explanation of failure, if any, don't set if NULL
 */
extern int  gres_plugin_node_config_validate(char *node_name,
					    char *orig_config,
					    char **new_config,
					    List *gres_list,
					    uint16_t fast_schedule,
					    char **reason_down)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find or create gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			list_append(*gres_list, gres_ptr);
		}

		rc = (*(gres_context[i].ops.node_config_validate))
			(node_name, orig_config, new_config,
			 &gres_ptr->gres_data, fast_schedule, reason_down);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Note that a node's configuration has been modified (e.g. "scontol update ..")
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT new_config - Updated gres info from slurm.conf if FastSchedule=0
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN fast_schedule - 0: Validate and use actual hardware configuration
 *		      1: Validate hardware config, but use slurm.conf config
 *		      2: Don't validate hardware, use slurm.conf configuration
 */
extern int gres_plugin_node_reconfig(char *node_name,
				     char *orig_config,
				     char **new_config,
				     List *gres_list,
				     uint16_t fast_schedule)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))){
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL)
			continue;

		rc = (*(gres_context[i].ops.node_reconfig))
			(node_name, orig_config, new_config,
			 &gres_ptr->gres_data, fast_schedule);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Pack a node's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_plugin_node_config_validate()
 * IN/OUT buffer - location to write state to
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_plugin_node_state_pack(List gres_list, Buf buffer,
				       char *node_name)
{
	int i, rc = SLURM_SUCCESS, rc2;
	uint32_t top_offset, gres_size = 0;
	uint32_t header_offset, size_offset, data_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (gres_list == NULL) {
		pack16(rec_cnt, buffer);
		return rc;
	}

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			header_offset = get_buf_offset(buffer);
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			size_offset = get_buf_offset(buffer);
			pack32(gres_size, buffer);	/* placeholder */
			data_offset = get_buf_offset(buffer);
			rc2 = (*(gres_context[i].ops.node_state_pack))
					(gres_ptr->gres_data, buffer);
			if (rc2 != SLURM_SUCCESS) {
				rc = rc2;
				set_buf_offset(buffer, header_offset);
				break;
			}
			tail_offset = get_buf_offset(buffer);
			set_buf_offset(buffer, size_offset);
			gres_size = tail_offset - data_offset;
			pack32(gres_size, buffer);
			set_buf_offset(buffer, tail_offset);
			rec_cnt++;
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to pack record for "
			      "node %s",
			      gres_ptr->plugin_id, node_name);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a node's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_plugin_node_state_pack()
 * IN/OUT buffer - location to read state from
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_plugin_node_state_unpack(List *gres_list, Buf buffer,
					 char *node_name)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;
	gres_state_t *gres_ptr;
	void *gres_data;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_node_state_unpack: no plugin "
			      "configured to unpack data type %u from node %s",
			      plugin_id, node_name);
			/* A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc2 = (*(gres_context[i].ops.node_state_unpack))
				(&gres_data, buffer);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the node. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		error("gres_plugin_node_state_unpack: no info packed for %s "
		      "by node %s",
		      gres_context[i].gres_type, node_name);
		rc2 = (*(gres_context[i].ops.node_state_unpack))
				(&gres_data, NULL);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_node_state_unpack: unpack error from node %s",
	      node_name);
	rc = SLURM_ERROR;
	goto fini;
}

/*
 * Duplicate a node gres status (used for will-run logic)
 * IN gres_list - node gres state information
 * RET a copy of gres_list or NULL on failure
 */
extern List gres_plugin_node_state_dup(List gres_list)
{
	int i;
	List new_list = NULL;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres;
	void *gres_data;

	if (gres_list == NULL)
		return new_list;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0)) {
		new_list = list_create(_gres_node_list_delete);
		if (new_list == NULL)
			fatal("list_create malloc failure");
	}
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			gres_data = (*(gres_context[i].ops.node_state_dup))
					(gres_ptr->gres_data);
			if (gres_data) {
				new_gres = xmalloc(sizeof(gres_state_t));
				new_gres->plugin_id = gres_ptr->plugin_id;
				new_gres->gres_data = gres_data;
				list_append(new_list, new_gres);
			}
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup node record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_list;
}

/*
 * Deallocate all resources on this node previous allocated to any jobs.
 *	This function isused to synchronize state after slurmctld restarts or
 *	is reconfigured.
 * IN gres_list - node gres state information
 */
extern void gres_plugin_node_state_dealloc(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (gres_list == NULL)
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			(*(gres_context[i].ops.node_state_dealloc))
					(gres_ptr->gres_data);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Allocate in this nodes record the resources previously allocated to this
 *	job. This function isused to synchronize state after slurmctld restarts
 *	or is reconfigured.
 * IN job_gres_list - job gres state information
 * IN node_offset - zero-origin index of this node in the job's allocation
 * IN node_gres_list - node gres state information
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_node_state_realloc(List job_gres_list, int node_offset,
					  List node_gres_list)
{
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;
	int i;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *)
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL) {
			error("Could not find plugin id %u to realloc job",
			      job_gres_ptr->plugin_id);
			continue;
		}

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			(*(gres_context[i].ops.node_state_realloc))
					(job_gres_ptr->gres_data, node_offset,
					 node_gres_ptr->gres_data);
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return SLURM_SUCCESS;
}

/*
 * Log a node's current gres state
 * IN gres_list - generated by gres_plugin_node_config_validate()
 * IN node_name - name of the node for which the gres information applies
 */
extern void gres_plugin_node_state_log(List gres_list, char *node_name)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!gres_debug || (gres_list == NULL))
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			(*(gres_context[i].ops.node_state_log))
					(gres_ptr->gres_data, node_name);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static void _gres_job_list_delete(void *list_element)
{
	int i;
	gres_state_t *gres_ptr;

	if (gres_plugin_init() != SLURM_SUCCESS)
		return;

	gres_ptr = (gres_state_t *) list_element;
	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_ptr->plugin_id != *(gres_context[i].ops.plugin_id))
			continue;
		(*(gres_context[i].ops.job_state_delete))(gres_ptr->gres_data);
		xfree(gres_ptr);
		break;
	}
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Given a job's requested gres configuration, validate it and build a gres list
 * IN req_config - job request's gres input string
 * OUT gres_list - List of Gres records for this job to track usage
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_plugin_job_state_validate(char *req_config, List *gres_list)
{
	char *tmp_str, *tok, *last = NULL;
	int i, rc, rc2;
	gres_state_t *gres_ptr;
	void *gres_data;

	if ((req_config == NULL) || (req_config[0] == '\0')) {
		*gres_list = NULL;
		return SLURM_SUCCESS;
	}

	if ((rc = gres_plugin_init()) != SLURM_SUCCESS)
		return rc;

	slurm_mutex_lock(&gres_context_lock);
	tmp_str = xstrdup(req_config);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_job_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	tok = strtok_r(tmp_str, ",", &last);
	while (tok && (rc == SLURM_SUCCESS)) {
		rc2 = SLURM_ERROR;
		for (i=0; i<gres_context_cnt; i++) {
			rc2 = (*(gres_context[i].ops.job_state_validate))
					(tok, &gres_data);
			if (rc2 != SLURM_SUCCESS)
				continue;
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
			break;		/* processed it */
		}
		if (rc2 != SLURM_SUCCESS) {
			info("Invalid gres job specification %s", tok);
			rc = ESLURM_INVALID_GRES;
			break;
		}
		tok = strtok_r(NULL, ",", &last);
	}
	slurm_mutex_unlock(&gres_context_lock);

	xfree(tmp_str);
	return rc;
}

/*
 * Create a copy of a job's gres state
 * IN gres_list - List of Gres records for this job to track usage
 * RET The copy or NULL on failure
 */
List gres_plugin_job_state_dup(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres_state;
	List new_gres_list = NULL;
	void *new_gres_data;

	if (gres_list == NULL)
		return new_gres_list;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			new_gres_data = (*(gres_context[i].ops.job_state_dup))
					(gres_ptr->gres_data);
			if (new_gres_data == NULL)
				break;
			if (new_gres_list == NULL) {
				new_gres_list = list_create(_gres_job_list_delete);
				if (new_gres_list == NULL)
					fatal("list_create: malloc failure");
			}
			new_gres_state = xmalloc(sizeof(gres_state_t));
			new_gres_state->plugin_id = gres_ptr->plugin_id;
			new_gres_state->gres_data = new_gres_data;
			list_append(new_gres_list, new_gres_state);
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup job record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_gres_list;
}

/*
 * Pack a job's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_plugin_job_config_validate()
 * IN/OUT buffer - location to write state to
 * IN job_id - job's ID
 */
extern int gres_plugin_job_state_pack(List gres_list, Buf buffer,
				      uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS, rc2;
	uint32_t top_offset, gres_size = 0;
	uint32_t header_offset, size_offset, data_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			header_offset = get_buf_offset(buffer);
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			size_offset = get_buf_offset(buffer);
			pack32(gres_size, buffer);	/* placeholder */
			data_offset = get_buf_offset(buffer);
			rc2 = (*(gres_context[i].ops.job_state_pack))
					(gres_ptr->gres_data, buffer);
			if (rc2 != SLURM_SUCCESS) {
				rc = rc2;
				set_buf_offset(buffer, header_offset);
				continue;
			}
			tail_offset = get_buf_offset(buffer);
			set_buf_offset(buffer, size_offset);
			gres_size = tail_offset - data_offset;
			pack32(gres_size, buffer);
			set_buf_offset(buffer, tail_offset);
			rec_cnt++;
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to pack record for "
			      "job %u",
			      gres_ptr->plugin_id, job_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a job's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_plugin_job_state_pack()
 * IN/OUT buffer - location to read state from
 * IN job_id - job's ID
 */
extern int gres_plugin_job_state_unpack(List *gres_list, Buf buffer,
					uint32_t job_id)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;
	gres_state_t *gres_ptr;
	void *gres_data;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_job_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_job_state_unpack: no plugin "
			      "configured to unpack data type %u from job %j",
			      plugin_id, job_id);
			/* A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc2 = (*(gres_context[i].ops.job_state_unpack))
				(&gres_data, buffer);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the job. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		debug("gres_plugin_job_state_unpack: no info packed for %s "
		      "by job %u",
		      gres_context[i].gres_type, job_id);
		rc2 = (*(gres_context[i].ops.job_state_unpack))
				(&gres_data, NULL);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_job_state_unpack: unpack error from job %u",
	      job_id);
	rc = SLURM_ERROR;
	goto fini;
}

/*
 * Determine how many CPUs on the node can be used by this job
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *                     gres_plugin_node_config_validate()
 * IN use_total_gres - if set then consider all gres resources as available,
 *		       and none are commited to running jobs
 * RET: NO_VAL    - All CPUs on node are available
 *      otherwise - Specific CPU count
 */
extern uint32_t gres_plugin_job_test(List job_gres_list, List node_gres_list,
				     bool use_total_gres)
{
	int i;
	uint32_t cpu_cnt, tmp_cnt;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return NO_VAL;
	if (node_gres_list == NULL)
		return NO_VAL;

	cpu_cnt = NO_VAL;
	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *)
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL) {
			/* node lack resources required by the job */
			cpu_cnt = 0;
			break;
		}

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			tmp_cnt = (*(gres_context[i].ops.job_test))
					(job_gres_ptr->gres_data,
					 node_gres_ptr->gres_data,
					 use_total_gres);
			cpu_cnt = MIN(tmp_cnt, cpu_cnt);
			break;
		}
		if (cpu_cnt == 0)
			break;
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return cpu_cnt;
}

/*
 * Allocate resource to a job and update node and job gres information
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *		gres_plugin_node_config_validate()
 * IN node_cnt - total number of nodes originally allocated to the job
 * IN node_offset - zero-origin index to the node of interest
 * IN cpu_cnt - number of CPUs allocated to this job on this node
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_job_alloc(List job_gres_list, List node_gres_list, 
				 int node_cnt, int node_offset,
				 uint32_t cpu_cnt)
{
	int i, rc, rc2;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *) 
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = (*(gres_context[i].ops.job_alloc))
					(job_gres_ptr->gres_data, 
					 node_gres_ptr->gres_data, node_cnt,
					 node_offset, cpu_cnt);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Deallocate resource from a job and update node and job gres information
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *		gres_plugin_node_config_validate()
 * IN node_offset - zero-origin index to the node of interest
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_job_dealloc(List job_gres_list, List node_gres_list, 
				   int node_offset)
{
	int i, rc, rc2;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *) 
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = (*(gres_context[i].ops.job_dealloc))
					(job_gres_ptr->gres_data, 
					 node_gres_ptr->gres_data, node_offset);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Log a job's current gres state
 * IN gres_list - generated by gres_plugin_job_state_validate()
 * IN job_id - job's ID
 */
extern void gres_plugin_job_state_log(List gres_list, uint32_t job_id)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!gres_debug || (gres_list == NULL))
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			(*(gres_context[i].ops.job_state_log))
					(gres_ptr->gres_data, job_id);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}
