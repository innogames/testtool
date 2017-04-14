#ifndef _PF_MECHANISM_H
#define _PF_MECHANISM_H

#include <string>

#include "mechanism.h"

/*
   pfctl-based production mechanism.
*/
class PfMechanism : public Mechanism {
public:
	PfMechanism(LbPool *lbpool);
	virtual ~PfMechanism();

	virtual void read_nodes();
	virtual void set_nodes();

};

#endif

