#ifndef _DUMMY_MECHANISM_H
#define _DUMMY_MECHANISM_H

#include <set>

#include "mechanism.h"

/*
   Dummy mechanism that merely prints what it would do.
*/
class DummyMechanism : public Mechanism {
public:
	DummyMechanism(LbVip* vip);
    virtual ~DummyMechanism();

    virtual void add_node(LbPoolLink* link, LbNode* node);
    virtual void del_node(LbPoolLink* link, LbNode* node);
    virtual bool has_node(LbPoolLink* link, LbNode* node);
    virtual void cleanup_orphans();

private:
	set<LbNode*> m_active_nodes;
};

#endif

