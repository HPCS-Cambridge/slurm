/*****************************************************************************\
 *  acct_gather_infiniband_ofed.c -slurm infiniband accounting plugin for ofed
 *****************************************************************************
 *  Copyright (C) 2013
 *  Written by Bull- Yiannis Georgiou
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
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


#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

#include <unistd.h>
#include <getopt.h>
#include <netinet/in.h>


#include "src/common/slurm_xlator.h"
#include "src/common/slurm_acct_gather_infiniband.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/slurmd/common/proctrack.h"
#include "src/common/slurm_acct_gather_profile.h"

#include "src/slurmd/slurmd/slurmd.h"
#include "acct_gather_infiniband_ofed.h"

/*
 * ofed includes for the lib
 */

#include <infiniband/umad.h>
#include <infiniband/mad.h>

/***************************************************************/

#define ALL_PORTS 0xFF


#define _DEBUG 1
#define _DEBUG_INFINIBAND 1
#define TIMEOUT 20
#define IB_FREQ 4

#define ERR_NO_IB_CARD -1337

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
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as the job accounting API
 * matures.
 */

const char plugin_name[] = "AcctGatherInfiniband OFED plugin";
const char plugin_type[] = "acct_gather_infiniband/ofed";
const uint32_t plugin_version = 100;

/* Prevent code messiness when allocating memory */
#define TRY_ALLOC(_toalloc, _size, _failtag) \
{ \
  if (!(_toalloc = xmalloc(_size))) { \
    goto _failtag; \
  } \
}

typedef struct {
  uint32_t cards;
	uint32_t *ports;
  char **names;
} slurm_ofed_conf_t;


struct ibmad_port **srcport = NULL;
static ib_portid_t *portid;
static int *ibd_timeout = NULL;
static int *port = NULL;

typedef struct {
	time_t last_update_time;
	time_t update_time;
	uint64_t xmtdata;
	uint64_t rcvdata;
	uint64_t xmtpkts;
	uint64_t rcvpkts;
	uint64_t total_xmtdata;
	uint64_t total_rcvdata;
	uint64_t total_xmtpkts;
	uint64_t total_rcvpkts;
} ofed_sens_t;

static ofed_sens_t *ofed_sens = NULL;

typedef struct {
  uint64_t xmtdata;
  uint64_t rcvdata;
  uint64_t xmtpkts;
  uint64_t rcvpkts;
} transcv_data;

static transcv_data *last_update = NULL;

static uint8_t pc[1024];

static uint16_t *cap_mask; // AT

static slurm_ofed_conf_t ofed_conf;
static uint64_t debug_flags = 0;
static pthread_mutex_t ofed_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *_slurm_pma_query_via(void *rcvbuf, ib_portid_t * dest, int port,
				     unsigned timeout, unsigned id,
				     const struct ibmad_port *srcport)
{
#ifdef HAVE_OFED_PMA_QUERY_VIA
	return pma_query_via(rcvbuf, dest, port, timeout, id, srcport);
#else
	switch (id) {
	case CLASS_PORT_INFO:
		return perf_classportinfo_query_via(
			pc, &portid, port, ibd_timeout, srcport);
		break;
	case IB_GSI_PORT_COUNTERS_EXT:
		return port_performance_ext_query_via(
			pc, &portid, port, ibd_timeout, srcport);
		break;
	default:
		error("_slurm_pma_query_via: unhandled id");
	}
	return NULL;
#endif
}

// TODO FIXME adhere to SLURM practices, cleanup, proper function prefixes etc
int _ofed_card_init(char *ib_card, int ib_port, transcv_data *trcv, uint16_t *cap_mask, int nr) {
  int mgmt_classes[4] = {IB_SMI_CLASS, IB_SMI_DIRECT_CLASS,
             IB_SA_CLASS, IB_PERFORMANCE_CLASS};
  srcport[nr] = mad_rpc_open_port(ib_card, ib_port,
            mgmt_classes, 4);
  if (!srcport[nr]){
    error("Failed to open '%s' port '%d'", ib_card,
          ib_port);
    debug("INFINIBAND: failed");
    return ERR_NO_IB_CARD;
  }

  if (ib_resolve_self_via(&portid[nr], &port[nr], 0, srcport[nr]) < 0)
    error("can't resolve self port %d", port[nr]);

  memset(pc, 0, sizeof(pc));
  if (!_slurm_pma_query_via(pc, &portid[nr], port[nr], ibd_timeout[nr],
        CLASS_PORT_INFO, srcport[nr]))
  error("classportinfo query: %m");

  memcpy(cap_mask, pc + 2, sizeof(*cap_mask));
  if (!_slurm_pma_query_via(pc, &portid[nr], port[nr], ibd_timeout[nr],
        IB_GSI_PORT_COUNTERS_EXT, srcport[nr])) {
    error("ofed: %m");
    return SLURM_ERROR;
  }

  mad_decode_field(pc, IB_PC_EXT_XMT_BYTES_F, &(trcv->xmtdata));
  mad_decode_field(pc, IB_PC_EXT_RCV_BYTES_F, &(trcv->rcvdata));
  mad_decode_field(pc, IB_PC_EXT_XMT_PKTS_F, &(trcv->xmtpkts));
  mad_decode_field(pc, IB_PC_EXT_RCV_PKTS_F, &(trcv->rcvpkts));

  if (debug_flags & DEBUG_FLAG_INFINIBAND)
    info("%s ofed init", plugin_name);

  return SLURM_SUCCESS;
}

/*
 * _read_ofed_values read the IB sensor and update last_update values and times
 */
static int _read_ofed_values(void)
{
	static bool first = true;

	int rc = SLURM_SUCCESS;
  int i;
  int card_init_rc = SLURM_SUCCESS;

	//uint16_t cap_mask;
	uint64_t send_val, recv_val, send_pkts, recv_pkts;

	if (first) {

    TRY_ALLOC(last_update, ofed_conf.cards*sizeof(transcv_data), rov_fail);
    TRY_ALLOC(srcport, ofed_conf.cards*sizeof(struct ibmad_port*), rov_fail);
    TRY_ALLOC(portid, ofed_conf.cards*sizeof(ib_portid_t), rov_fail);
    TRY_ALLOC(port, ofed_conf.cards*sizeof(int), rov_fail);
    TRY_ALLOC(ibd_timeout, ofed_conf.cards*sizeof(int), rov_fail);
    TRY_ALLOC(cap_mask, ofed_conf.cards*sizeof(uint16_t), rov_fail);

    /* Need to calloc <ofed_conf.cards> ofed_sens structs. */
    TRY_ALLOC(ofed_sens, ofed_conf.cards * sizeof(ofed_sens_t), rov_fail); // xmalloc is calloc, xmalloc_nz is malloc. Obvs.
    for (i = 0; i < ofed_conf.cards; i++) {
      ofed_sens[i].update_time = time(NULL);
    }

    for (i = 0; i < ofed_conf.cards; i++) {
      card_init_rc = _ofed_card_init(ofed_conf.names[i], ofed_conf.ports[i],
            &last_update[i], &cap_mask[i], i);
      if (ERR_NO_IB_CARD == card_init_rc) {
        if(i == 0) {
          card_init_rc = _ofed_card_init(NULL, INFINIBAND_DEFAULT_PORT, &last_update[0], &cap_mask[0], 0);
          if (SLURM_SUCCESS == card_init_rc) {
            debug3("Configured using INFINIBAND_DEFAULT_PORT");
            ofed_conf.cards = 1;
          }
          else {
            ofed_conf.cards = 0;
          }
        }
        else {
          // Bail out & only accept the first n cards that did initialise
          debug3("Card %s:%d [%d] could not be initialised. Continuing with %d cards.",
              ofed_conf.names[i], ofed_conf.ports[i], i, i);
          ofed_conf.cards = i;
        }
        break;
      }
      else if (SLURM_ERROR == card_init_rc) {
        debug("Unknown card init error");
      }
    }

    first = false;
    return SLURM_SUCCESS;

rov_fail:
    debug("Memory allocation failed");
    xfree(last_update);
    xfree(srcport);
    xfree(portid);
    xfree(port);
    xfree(ibd_timeout);
    xfree(ofed_sens);
    return SLURM_ERROR;

  }

  for (i = 0; i < ofed_conf.cards; i++) {
    ofed_sens[i].last_update_time = ofed_sens[i].update_time;
    ofed_sens[i].update_time = time(NULL);
  }

  // TODO VERIFY is cap_mask even used?
  for (i = 0; i < ofed_conf.cards; i++) {

    memset(pc, 0, sizeof(pc));
    memcpy(&cap_mask[i], pc + 2, sizeof(cap_mask[i]));
    if (!_slurm_pma_query_via(pc, &portid[i], port[i], ibd_timeout[i],
            IB_GSI_PORT_COUNTERS_EXT, srcport[i])) {
      error("ofed: %m [%s:%d]", ofed_conf.names[i], ofed_conf.ports[i]);
      return SLURM_ERROR;
    }

    mad_decode_field(pc, IB_PC_EXT_XMT_BYTES_F, &send_val);
    mad_decode_field(pc, IB_PC_EXT_RCV_BYTES_F, &recv_val);
    mad_decode_field(pc, IB_PC_EXT_XMT_PKTS_F, &send_pkts);
    mad_decode_field(pc, IB_PC_EXT_RCV_PKTS_F, &recv_pkts);

    ofed_sens[i].xmtdata = (send_val - last_update[i].xmtdata) * 4;
    ofed_sens[i].total_xmtdata += ofed_sens[i].xmtdata;
    ofed_sens[i].rcvdata = (recv_val - last_update[i].rcvdata) * 4;
    ofed_sens[i].total_rcvdata += ofed_sens[i].rcvdata;
    ofed_sens[i].xmtpkts = send_pkts - last_update[i].xmtpkts;
    ofed_sens[i].total_xmtpkts += ofed_sens[i].xmtpkts;
    ofed_sens[i].rcvpkts = recv_pkts - last_update[i].rcvpkts;
    ofed_sens[i].total_rcvpkts += ofed_sens[i].rcvpkts;

    last_update[i].xmtdata = send_val;
    last_update[i].rcvdata = recv_val;
    last_update[i].xmtpkts = send_pkts;
    last_update[i].rcvpkts = recv_pkts;
  }

	return rc;
}


/*
 * _thread_update_node_energy calls _read_ipmi_values and updates all values
 * for node consumption
 */
static int _update_node_infiniband(void)
{
	acct_network_data_t net[3];
	int rc = SLURM_SUCCESS;
  int i;

	slurm_mutex_lock(&ofed_lock);
	rc = _read_ofed_values();

  /* This little check is REQUIRED for some reason -- TODO find out what breaks */
  if (!ofed_sens) {
	  slurm_mutex_unlock(&ofed_lock);
    return SLURM_ERROR;
  }

	//memset(&net, 0, sizeof(acct_network_data_t));

  for (i = 0; i < ofed_conf.cards; i++) {
	  memset(&net[i], 0, sizeof(acct_network_data_t));
    net[i].packets_in = ofed_sens[i].rcvpkts;
    net[i].packets_out = ofed_sens[i].xmtpkts;
    net[i].size_in = (double) ofed_sens[i].rcvdata / 1048576;
    net[i].size_out = (double) ofed_sens[i].xmtdata / 1048576;
    acct_gather_profile_g_add_sample_data(ACCT_GATHER_PROFILE_NETWORK,
                  &net[i]);

    if (debug_flags & DEBUG_FLAG_INFINIBAND) {
      debug3("%d sec, transmitted %"PRIu64" bytes, "
           "received %"PRIu64" bytes",
           (int) (ofed_sens[i].update_time - ofed_sens[i].last_update_time),
           ofed_sens[i].xmtdata, ofed_sens[i].rcvdata);
    }
  }
	slurm_mutex_unlock(&ofed_lock);

	return rc;
}

static bool _run_in_daemon(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = 1;
		run = run_in_daemon("slurmstepd");
	}

	return run;
}


/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	debug_flags = slurm_get_debug_flags();

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	if (!_run_in_daemon())
		return SLURM_SUCCESS;

	if (srcport) {
		//_update_node_infiniband();
    for(int i = 0; i < ofed_conf.cards; i++) { //TODO hangs now for some reason
      if(srcport[i]) {
		    mad_rpc_close_port(srcport[i]);
      }
    }
	}

	if (debug_flags & DEBUG_FLAG_INFINIBAND)
		info("ofed: ended");

	return SLURM_SUCCESS;
}

extern int acct_gather_infiniband_p_node_update(void)
{
	uint32_t profile;
	int rc = SLURM_SUCCESS;
	static bool set = false;
	static bool run = true;

	if (!set) {
		set = true;
		acct_gather_profile_g_get(ACCT_GATHER_PROFILE_RUNNING,
					  &profile);

		if (!(profile & ACCT_GATHER_PROFILE_NETWORK))
			run = false;
	}

	if (run) {
		_update_node_infiniband();
  }

	return rc;
}

// TODO: docs, correct placement, prototype, safety/checking
int acct_gather_infiniband_parse_ofed_config(char *config)
{
  char *conf_str = xstrdup(config);
  char *token = NULL;
  char **cards = NULL;
  int count = 0;
  int i;

  token = strtok(conf_str, ",");
  do {
    count++;
  cards = xrealloc(cards, count * sizeof(char*)); // TODO check
  cards[count-1] = xstrdup(token);
  } while ((token = strtok(NULL, ",")));

  xfree(conf_str);

  ofed_conf.cards = count;
  TRY_ALLOC(ofed_conf.names, count * sizeof(char*), parse_fail);
  TRY_ALLOC(ofed_conf.ports, count * sizeof(int), parse_fail);

  for(i = 0; i < count; i++) {
    token = strtok(cards[i], ":");
    ofed_conf.names[i] = xstrdup(token);

    token = strtok(NULL, ":");
    ofed_conf.ports[i] = atoi(token);

    debug3("Card %s:port %d", ofed_conf.names[i], ofed_conf.ports[i]);

    xfree(cards[i]);
  }

  xfree(cards);

  return SLURM_SUCCESS;

parse_fail:
  xfree(ofed_conf.names);
  xfree(ofed_conf.ports);
  return SLURM_ERROR;
}

extern void acct_gather_infiniband_p_conf_set(s_p_hashtbl_t *tbl)
{
  char *ofed_config = NULL;

	if (tbl) {
    if (!s_p_get_string(&ofed_config, "InfinibandOFEDConfig", tbl)
         || acct_gather_infiniband_parse_ofed_config(ofed_config)) { //ofed_conf as param?
       debug3("Incorrect InfinibandOFEDConfig value: %s", ofed_config);

       ofed_conf.ports = xmalloc(sizeof(int));
       ofed_conf.names = xmalloc(sizeof(char*));
       ofed_conf.cards = 1;

       if (!s_p_get_uint32(&(ofed_conf.ports[0]), "InfinibandOFEDPort", tbl)) {
         ofed_conf.ports[0] = INFINIBAND_DEFAULT_PORT;
       }

     }

    if (ofed_config) {
      xfree(ofed_config);
    }
	}

	if (!_run_in_daemon())
		return;

	debug("%s loaded", plugin_name);
}

extern void acct_gather_infiniband_p_conf_options(s_p_options_t **full_options,
						  int *full_options_cnt)
{
	s_p_options_t options[] = {
		{"InfinibandOFEDPort", S_P_UINT32},
		{"InfinibandOFEDConfig", S_P_STRING},
		{NULL} };

	transfer_s_p_options(full_options, options, full_options_cnt);

	return;
}

extern void acct_gather_infiniband_p_conf_values(List *data)
{
	config_key_pair_t *key_pair;

	xassert(*data);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("InfinibandOFEDPort");
	key_pair->value = xstrdup_printf("%u", ofed_conf.ports[0]);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("InfinibandOFEDConfig");
	key_pair->value = xstrdup_printf("TODO");//TODO
	list_append(*data, key_pair);

	return;
}
