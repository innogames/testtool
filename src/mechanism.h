#ifndef _MECHANISM_H
#define _MECHANISM_H

#include <map>
#include <set>

class LbPool;

using namespace std;

/*
   A mechanism actually implements the load balancing logic.
   It is responsible for registering nodes on whatever load balancing logic is used underneath.

   Node updates are always grouped by pool to allow link-specific mechanism logic.
   (For example, one might have special link types that take into account the source IP.)
*/
class Mechanism {
	protected:
		LbPool* parent_lbpool;

	public:
		Mechanism(LbPool *lbpool);
		virtual ~Mechanism();
		virtual void read_nodes();
		virtual void set_nodes();
};

#endif
