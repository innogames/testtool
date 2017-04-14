#ifndef _DUMMY_MECHANISM_H
#define _DUMMY_MECHANISM_H

#include <set>

#include "mechanism.h"

/*
   Dummy mechanism that merely prints what it would do.
*/

class DummyMechanism : public Mechanism {
public:
	DummyMechanism(LbPool *lbpool);
	virtual ~DummyMechanism();

	virtual void read_nodes();
	virtual void set_nodes();

};

#endif

