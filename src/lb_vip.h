#ifndef _LB_VIP_H_
#define _LB_VIP_H_

#include <iostream>
#include <set>
#include <vector>

#include "lb_pool.h"
#include "mechanism.h"

using namespace std;

class LbVip;

enum PoolType {
	POOL_PRIMARY = 0,
	POOL_BACKUP,
};

/**
 * Association between VIP and any related pools.
 */
struct LbPoolLink {
	LbVip*   vip;
	LbPool*  pool;
	Mechanism* mechanism;
	PoolType type;
	bool     active;
};

/**
 * Virtual IP with forwarded ports.
 */
class LbVip {
	/* Methods */
	public:
		LbVip(string name, string hwlb, string proto);

		void attach_pool(LbPool* pool, PoolType type);
		void notify_pool_update(LbPoolLink* link);

		size_t count_live_nodes();
		LbPoolLink* get_backup_pool();
		LbPoolLink* get_primary_pool();

		void start();

	private:
		void enable_backup_pool(bool enable);

	/* Members */
	public:
		string name;
		string hwlb;
		string ipaddress;
		string proto;
	private:
		bool m_started;
		Mechanism* m_mechanism;
		vector<LbPoolLink*> m_pools;

		/* Track currently active nodes, which will be compared to the desired state
         * every time the active pool changes. */
		set<LbNode*> m_active_nodes;
};

#endif

