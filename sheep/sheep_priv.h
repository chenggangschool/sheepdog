/*
 * Copyright (C) 2009-2010 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __SHEEP_PRIV_H__
#define __SHEEP_PRIV_H__

#include <inttypes.h>
#include <corosync/cpg.h>

#include "sheepdog_proto.h"
#include "event.h"
#include "logger.h"
#include "work.h"
#include "net.h"
#include "sheep.h"

#define SD_DEFAULT_REDUNDANCY 3
#define SD_MAX_REDUNDANCY 8

#define SD_OP_REMOVE_OBJ     0x91

#define SD_OP_GET_OBJ_LIST   0xA1

#define SD_MSG_JOIN             0x01
#define SD_MSG_VDI_OP           0x02
#define SD_MSG_MASTER_CHANGED   0x03

#define SD_STATUS_OK                0x00
#define SD_STATUS_WAIT_FOR_FORMAT   0x01
#define SD_STATUS_WAIT_FOR_JOIN     0x02
#define SD_STATUS_SHUTDOWN          0x03
#define SD_STATUS_JOIN_FAILED       0x04

enum cpg_event_type {
	CPG_EVENT_CONCHG,
	CPG_EVENT_DELIVER,
	CPG_EVENT_REQUEST,
};

struct cpg_event {
	enum cpg_event_type ctype;
	struct list_head cpg_event_list;
	unsigned int skip;
};

struct client_info {
	struct connection conn;

	struct request *rx_req;

	struct request *tx_req;

	struct list_head reqs;
	struct list_head done_reqs;

	int refcnt;
};

struct request;

typedef void (*req_end_t) (struct request *);

struct request {
	struct cpg_event cev;
	struct sd_req rq;
	struct sd_rsp rp;

	void *data;

	struct client_info *ci;
	struct list_head r_siblings;
	struct list_head r_wlist;
	struct list_head pending_list;

	uint64_t local_oid[2];

	struct sheepdog_node_list_entry entry[SD_MAX_NODES];
	int nr_nodes;

	req_end_t done;
	struct work work;
};

struct cluster_info {
	cpg_handle_t handle;
	/* set after finishing the JOIN procedure */
	int join_finished;
	uint32_t this_nodeid;
	uint32_t this_pid;
	struct sheepdog_node_list_entry this_node;

	uint32_t epoch;
	uint32_t status;

	/*
	 * we add a node to cpg_node_list in confchg then move it to
	 * sd_node_list when the node joins sheepdog.
	 */
	struct list_head cpg_node_list;
	struct list_head sd_node_list;

	struct list_head vm_list;
	struct list_head pending_list;

	DECLARE_BITMAP(vdi_inuse, SD_NR_VDIS);

	struct list_head outstanding_req_list;
	struct list_head req_wait_for_obj_list;

	uint32_t nr_sobjs;

	struct list_head cpg_event_siblings;
	struct cpg_event *cur_cevent;
	unsigned long cpg_event_work_flags;
	int nr_outstanding_io;
};

extern struct cluster_info *sys;

int create_listen_port(int port, void *data);

int is_io_request(unsigned op);
int init_store(const char *dir);

int add_vdi(uint32_t epoch, char *data, int data_len, uint64_t size,
	    uint32_t *new_vid, uint32_t base_vid, uint32_t copies,
	    int is_snapshot);

int del_vdi(uint32_t epoch, char *data, int data_len, uint32_t *vid, uint32_t snapid);

int lookup_vdi(uint32_t epoch, char *data, int data_len, uint32_t *vid, uint32_t snapid);

int read_vdis(char *data, int len, unsigned int *rsp_len);

int setup_ordered_sd_node_list(struct request *req);
int get_ordered_sd_node_list(struct sheepdog_node_list_entry *entries);
int is_access_to_busy_objects(uint64_t oid);

void resume_pending_requests(void);

int create_cluster(int port);

void start_cpg_event_work(void);
void store_queue_request(struct work *work, int idx);

int read_epoch(uint32_t *epoch, uint64_t *ctime,
	       struct sheepdog_node_list_entry *entries, int *nr_entries);
void cluster_queue_request(struct work *work, int idx);

int update_epoch_store(uint32_t epoch);

int set_global_nr_copies(uint32_t copies);
int get_global_nr_copies(uint32_t *copies);
int set_nodeid(uint64_t nodeid);
int get_nodeid(uint64_t *nodeid);

#define NR_WORKER_THREAD 64

int epoch_log_write(uint32_t epoch, char *buf, int len);
int epoch_log_read(uint32_t epoch, char *buf, int len);
int get_latest_epoch(void);
int remove_epoch(int epoch);
int set_cluster_ctime(uint64_t ctime);
uint64_t get_cluster_ctime(void);

int start_recovery(uint32_t epoch, uint32_t *failed_vdis, int nr_failed_vdis);
void resume_recovery_work(void);
int is_recoverying_oid(uint64_t oid);

int write_object(struct sheepdog_node_list_entry *e,
		 int nodes, uint32_t node_version,
		 uint64_t oid, char *data, unsigned int datalen,
		 uint64_t offset, int nr, int create);
int read_object(struct sheepdog_node_list_entry *e,
		int nodes, uint32_t node_version,
		uint64_t oid, char *data, unsigned int datalen,
		uint64_t offset, int nr);
int remove_object(struct sheepdog_node_list_entry *e,
		  int nodes, uint32_t node_version,
		  uint64_t oid, int nr);

static inline int is_myself(struct sheepdog_node_list_entry *e)
{
	return e->id == sys->this_node.id;
}

#define panic(fmt, args...)			\
({						\
	vprintf(SDOG_EMERG fmt, ##args);	\
	exit(1);				\
})

#endif