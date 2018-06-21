#ifndef _MECHANISM_H
#define _MECHANISM_H

#include <map>
#include <set>

#include "lb_node.h"

class LbVip;
struct LbPoolLink;

using namespace std;

/*
   A mechanism actually implements the load balancing logic.
   It is responsible for registering nodes on whatever load balancing logic is used underneath.

   Node updates are always grouped by pool to allow link-specific mechanism logic.
   (For example, one might have special link types that take into account the source IP.)
*/
class Mechanism {
public:
	Mechanism(LbVip* vip);
	virtual ~Mechanism();
	virtual void update_nodes(LbPoolLink* link, const set<LbNode*>& add_nodes, const set<LbNode*>& del_nodes); /* Invokes add_node/del_node, but may do clever batch processing. */
	virtual void add_node(LbPoolLink* link, LbNode* node) = 0;
	virtual void del_node(LbPoolLink* link, LbNode* node) = 0;
	/* Returns the actual hardware state, regardless of the in-memory state. */
	virtual bool has_node(LbPoolLink* link, LbNode* node) = 0;

	/* Removes nodes from the hardware state that aren't part of the current mechanism state. */
	virtual void cleanup_orphans() = 0;

	/* Determines the actual state of the node (it might have been added before a restart) and registers the node, if necessary.
	   Returns whether the node is already added. */
	bool sync_initial_state(LbPoolLink* link, LbNode* node);

	/* Updates the set of active nodes for a given link. The implementation will invoke update_nodes with the delta. */
	void sync_nodes(LbPoolLink* link, const set<LbNode*>& new_nodes);

protected:
	LbVip* m_vip;
	map<LbPoolLink*, set<LbNode*>> m_active_nodes;
};

#endif
