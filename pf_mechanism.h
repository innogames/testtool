#ifndef _PF_MECHANISM_H
#define _PF_MECHANISM_H

#include <string>

#include "mechanism.h"

/*
   pfctl-based production mechanism.
*/
class PfMechanism : public Mechanism {
public:
	PfMechanism(LbVip* vip);
	virtual ~PfMechanism();

	virtual void update_nodes(LbPoolLink* link, const set<LbNode*>& add_nodes, const set<LbNode*>& del_nodes);
	virtual void add_node(LbPoolLink* link, LbNode* node);
	virtual void del_node(LbPoolLink* link, LbNode* node);
	virtual bool has_node(LbPoolLink* link, LbNode* node);
	virtual void cleanup_orphans();

private:
	void del_address(string& address);
};

#endif

