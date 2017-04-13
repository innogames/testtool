#include <iostream>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

#include "lb_vip.h"
#include "lb_node.h"
#include "dummy_mechanism.h"
#include "pf_mechanism.h"

using namespace std;


LbVip::LbVip(string name, string hwlb, string proto)
	: name(name), hwlb(hwlb), m_started(false)
{
	m_mechanism = new PfMechanism(this);
	this -> proto = proto;
	log_txt(MSG_TYPE_DEBUG, "* New LbVip %s", name.c_str());
}


/*
   Attaches a pool to the VIP.
*/
void LbVip::attach_pool(LbPool* pool, PoolType type) {
	/* All pools start inactive to avoid mechanism actions during startup. */
	auto link = new LbPoolLink({this, pool, m_mechanism, type, false});
	pool->vips.push_back(link);
	m_pools.push_back(link);

	notify_pool_update(link);
}


/*
   Handle the state change of an attached pool.
*/
void LbVip::notify_pool_update(LbPoolLink* link) {
	if (!m_started) {
		return;
	}

	/* State changes of primary pool trigger backup switch. */
	if (link->type == POOL_PRIMARY) {
		if (link->active && link->pool->state == LbPool::STATE_DOWN) {
			/* Primary pool went down, enable backup pools. */
			enable_backup_pool(true);

			link->active = false;
			m_mechanism->sync_nodes(link, link->pool->active_nodes());
			return;
		} else if (!link->active && link->pool->state == LbPool::STATE_UP) {
			/* Primary pool went back up, switch back to primary. */

			/* First re-enable the primary pool, then remove backup, to avoid race conditions. */
			link->active = true;
			m_mechanism->sync_nodes(link, link->pool->active_nodes());

			enable_backup_pool(false);
			return;
		}
	}

	if (!link->active) {
		/* We don't care about inactive pools going nuts. */
		return;
	}

	/* If there's no special case, just pass the new node list to the mechanism. */
	m_mechanism->sync_nodes(link, link->pool->active_nodes());
}


/*
   Returns the configured backup pool, or NULL.
*/
LbPoolLink* LbVip::get_backup_pool() {
	for (auto link : m_pools) {
		if (link->type == POOL_BACKUP) {
			return link;
		}
	}
	return nullptr;
}


/*
   Returns the configured primary pool, or NULL.
*/
LbPoolLink* LbVip::get_primary_pool() {
	for (auto link : m_pools) {
		if (link->type == POOL_PRIMARY) {
			return link;
		}
	}
	return nullptr;
}


void LbVip::enable_backup_pool(bool enable) {
	auto link = get_backup_pool();
	if (link) {
		log_txt(MSG_TYPE_DEBUG, "Turning %s backup pool %s for VIP %s", enable?"ON":"OFF", link->pool->name.c_str(), link->vip->name.c_str());

		link->active = enable;
		m_mechanism->sync_nodes(link, link->pool->active_nodes());
	}
}


/*
Returns the total number of UP nodes attached to this VIP.
*/
size_t LbVip::count_live_nodes() {
	size_t sum = 0;
	for (auto link : m_pools) {
		if (link->active) {
			sum += link->pool->count_live_nodes();
		}
	}
	return sum;
}


void LbVip::start() {
	m_started = true;
	for (auto link : m_pools) {
		/* This will automatically enable a suitable pool. */
		notify_pool_update(link);
	}
	m_mechanism->cleanup_orphans();
}
