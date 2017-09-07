#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <iostream>
#include <fmt/format.h>
#include <boost/interprocess/ipc/message_queue.hpp>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_worker.h"

using namespace std;
using namespace boost::interprocess;

pid_t worker_pid;

void pfctl_worker_loop(message_queue* mq) {
	pfctl_msg msg;
	message_queue::size_type recvd_size;
	unsigned int priority;

	log(MSG_INFO, "Entering pfctl worker's loop");
	setproctitle("%s", "pfctl worker");

	while (true) {
		// Normal receive won't stop blocking when the queue
		// is removed on master process.
		try {
			mq->receive(&msg, sizeof(pfctl_msg), recvd_size, priority);
		}
		catch(interprocess_exception &ex){
			message_queue::remove("pfctl");
			log(MSG_INFO, fmt::sprintf("Exception in pfctl worker %s", ex.what()));
			return;
		}
		assert (recvd_size == sizeof(pfctl_msg));

		log(MSG_INFO, fmt::sprintf("lbpool: %s starting synchronizing pf table %s", msg.pool_name, msg.table_name));

		// Decode the message
		string table_name(msg.table_name);
		set<string> wanted_addresses;

		for (int i = 0; i < MAX_NODES; i++) {
			string wanted_address(msg.wanted_addresses[i]);
			if (wanted_address.length())
				wanted_addresses.insert(wanted_address);
		}

		/*
		 * Measure total time of all pfctl operations.
		 * Don't use std::chrono, there is a conflict with boost.
		 */
		chrono::high_resolution_clock::time_point t1 = chrono::high_resolution_clock::now();
		pf_sync_table(table_name, wanted_addresses);
		chrono::high_resolution_clock::time_point t2 = chrono::high_resolution_clock::now();
		auto duration = chrono::duration_cast<chrono::milliseconds>(t2 - t1).count();
		log(MSG_INFO, fmt::sprintf("lbpool: %s finished synchronizing pf table %s time: %dms", msg.pool_name, msg.table_name, duration));
	}
	message_queue::remove("pfctl");
}

message_queue* new_pfctl_queue(bool create_q) {

	message_queue* rmq = NULL;

	if (create_q) {
		try {
			static message_queue mq(
				create_only,
				"pfctl",
				QUEUE_LEN,
				sizeof(pfctl_msg)
			);
			rmq = &mq;
		}
		catch(const runtime_error& ex)
		{
			// speciffic handling for runtime_error
			log(MSG_INFO, fmt::sprintf("Exception in pfctl worker %s", ex.what()));
		}
		catch(const exception& ex)
		{
			// std::runtime_error which is handled explicitly
			log(MSG_INFO, fmt::sprintf("Exception in pfctl worker %s", ex.what()));
		}
	} else {
		while (true) {
			/*
			 * Default condition is that waiting is finished with
			 * queue found or not.
			 */
			try {
				static message_queue mq(
					open_only,
					"pfctl"
				);
				rmq = &mq;
			}
			catch(const interprocess_exception& ex)
			{
				if (ex.get_error_code() == 7) {
					// Awaiting master process to create the queue
					log(MSG_INFO, fmt::sprintf("Parent queue not created yet %s", ex.what()));
					usleep(100000);
					continue;
				} else {
					log(MSG_INFO, fmt::sprintf("Interprocess Exception in pfctl worker %d", ex.get_error_code()));
				}
			}
			catch(const runtime_error& ex)
			{
				// speciffic handling for runtime_error
				log(MSG_INFO, fmt::sprintf("Exception in pfctl worker %s", ex.what()));
			}
			catch(const exception& ex)
			{
				// std::runtime_error which is handled explicitly
				log(MSG_INFO, fmt::sprintf("Exception in pfctl worker %s", ex.what()));
			}
			break;
		}
	}

	return rmq;
}

void send_message(message_queue* mq, string pool_name, string table_name, set<LbNode*> lb_nodes) {
	pfctl_msg msg;

	memset(&msg, 0, sizeof(msg));
	strncpy(msg.pool_name, pool_name.c_str(), sizeof(msg.pool_name));
	strncpy(msg.table_name, table_name.c_str(), sizeof(msg.table_name));
	int node_index = 0;
	assert(lb_nodes.size() < MAX_NODES);
	for (auto node: lb_nodes) {
		strncpy(msg.wanted_addresses[node_index], node->address.c_str(), ADDR_LEN);
		node_index++;
	}
	mq->send(&msg, sizeof(pfctl_msg), 0);
}

message_queue* start_pfctl_worker() {
	pid_t pid;

	/*
	 * The queue must be opened in both processes and accessed by name.
	 * It must not be created before forking!
	 */
	message_queue::remove("pfctl");
	pid = fork();
	if (pid == 0) {
		// Child process
		message_queue* mq = new_pfctl_queue(false);
		log(MSG_INFO, fmt::sprintf("MQ attached in pfctl worker"));
		if (mq == NULL) {
			log(MSG_CRIT, "Unable to initialize pfctl worker, terminating!");
			exit(-1);
		}
		pfctl_worker_loop(mq);
		log(MSG_INFO, fmt::sprintf("Pfctl worker finished"));
		exit(EXIT_SUCCESS);
	} else if (pid == -1) {
		log(MSG_CRIT, "Unable to fork, terminating!");
		exit(-1);
	} else {
		worker_pid = pid;
		// Parent process
		message_queue* mq = new_pfctl_queue(true);
		if (mq == NULL) {
			log(MSG_CRIT, "Unable to create pfctl queue, terminating!");
			exit(-1);
		}
		log(MSG_INFO, fmt::sprintf("MQ created in master thread"));
		return mq;
	}
}

void stop_pfctl_worker() {
	log(MSG_INFO, "Stopping pfctl worker");
	message_queue::remove("pfctl");
	log(MSG_INFO, "Queue removed from master process");
	kill(worker_pid, 15);
}
