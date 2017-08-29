/*
 * Testtool PostgreSQL Health Check
 *
 * Allow the PostgreSQL connections to be load-balanced by checking
 * the health of the database.
 *
 * Copyright (c) 2016, InnoGames GmbH
 */

#include <iostream>
#include <sstream>
#include <vector>
#include <cassert>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include <errno.h>

#include <event2/event.h>

#ifdef __FreeBSD__
#include <libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif

#include "config.h"
#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_postgres.h"

using namespace std;

extern struct event_base	*eventBase;
extern SSL_CTX			*sctx;
extern int			 verbose;

/*
 * The constructor
 *
 * We are initialising the variables, only.  Nothing should be able
 * to fail in there.
 */
Healthcheck_postgres::Healthcheck_postgres(const YAML::Node& config,
		class LbNode *_parent_lbnode): Healthcheck(config, _parent_lbnode) {

	this->type = "postgres";

	// Set defaults
	if (this->port == 0)
		this->port = 5432;

	this->host = parse_string(config["host"], parent_lbnode->address);
	this->dbname = parse_string(config["dbname"], "");
	this->user = parse_string(config["user"], "");
	this->function = parse_string(config["function"], "");

	this->prepare_query();
	log(MSG_INFO, this, fmt::sprintf("new healthcheck, query: %s", this->query));
}


/*
 * The entrypoint of the class
 *
 * This starts the health check by choosing the first step.  After we
 * call the method for the first step, things will continue in
 * an asynchronous chain of events.
 *
 * XXX Nobody checks the exit code of this function.  We must never
 * fail to call the first step for process to reach the end.
 */
int Healthcheck_postgres::schedule_healthcheck(struct timespec *now) {

	// Perform general stuff for scheduled health check
	if (!Healthcheck::schedule_healthcheck(now))
		return false;

	this->event_counter = 0;
	this->register_timeout_event();

	// The first step
	this->start_conn();

	return true;
}

/*
 * Connect or reconnect if necessary and continue
 *
 * This is the part of libpq documentation we are implementing:
 *
 * > To begin a nonblocking connection request, call
 * > conn = PQconnectStart("connection_info_string").  If conn is null,
 * > then libpq has been unable to allocate a new PGconn structure.
 * > Otherwise, a valid PGconn pointer is returned (though not yet
 * > representing a valid connection to the database).  On return from
 * > PQconnectStart, call status = PQstatus(conn).  If status equals
 * > CONNECTION_BAD, PQconnectStart has failed.
 */
void Healthcheck_postgres::start_conn() {
	std::string port_str = std::to_string(this->port);
	char const *keys[] = {"host", "port", "dbname", "user",
			      "client_encoding", "fallback_application_name",
			      NULL};
	char const *values[] = {this->host.c_str(), port_str.c_str(),
				this->dbname.c_str(), this->user.c_str(),
				"SQL_ASCII", "testtool", NULL};
	this->conn = PQconnectStartParams(keys, values, 0);

	// If it is NULL, the memory allocation must have been failed.
	if (this->conn == NULL)
		return this->end_check(HC_PANIC, "cannot start db connection");

	// The connection can fail right away.
	if (PQstatus(this->conn) == CONNECTION_BAD)
		return this->end_check(HC_FAIL, "db connection failed");

	if (PQsetnonblocking(this->conn, 1))
		return this->end_check(HC_PANIC, "cannot non-block db connection");

	// The next step
	this->register_io_event(EV_WRITE, &Healthcheck_postgres::poll_conn);
}

/*
 * Recursively poll the connection and continue
 *
 * This is the part of libpq documentation we are implementing:
 *
 * > If PQconnectStart succeeds, the next stage is to poll libpq so that
 * > it can proceed with the connection sequence.  Use PQsocket(conn) to
 * > obtain the descriptor of the socket underlying the database
 * > connection.  Loop thus: If PQconnectPoll(conn) last returned
 * > PGRES_POLLING_READING, wait until the socket is ready to read (as
 * > indicated by select(), poll(), or similar system function).  Then
 * > call PQconnectPoll(conn) again. Conversely, if PQconnectPoll(conn)
 * > last returned PGRES_POLLING_WRITING, wait until the socket is ready
 * > to write, then call PQconnectPoll(conn) again.  If you have yet to
 * > call PQconnectPoll, i.e., just after the call to PQconnectStart,
 * > behave as if it last returned PGRES_POLLING_WRITING.  Continue this
 * > loop until PQconnectPoll(conn) returns PGRES_POLLING_FAILED,
 * > indicating the connection procedure has failed, or PGRES_POLLING_OK,
 * > indicating the connection has been successfully made.
 */
void Healthcheck_postgres::poll_conn() {
	assert(PQstatus(this->conn) != CONNECTION_OK);
	assert(PQstatus(this->conn) != CONNECTION_BAD);
	assert(PQisnonblocking(this->conn));

	switch(PQconnectPoll(this->conn)) {
		case PGRES_POLLING_READING:
			this->register_io_event(EV_READ,
						&Healthcheck_postgres::poll_conn);
			break;

		case PGRES_POLLING_WRITING:
			this->register_io_event(EV_WRITE,
						&Healthcheck_postgres::poll_conn);
			break;

		case PGRES_POLLING_FAILED:
			this->end_check(HC_FAIL, "db connection polling failed");
			break;

		default:
			// There are no more used states than ok.
			assert(PQconnectPoll(this->conn) == PGRES_POLLING_OK);

			// The next step
			this->send_query();
	}
}

/*
 * Prepare the database query
 *
 * Currently, we only know how to call a function.  We could prepare
 * the function in server-side, but it doesn't worth the effort.
 */
void Healthcheck_postgres::prepare_query() {
	snprintf(this->query, sizeof(this->query), "SELECT %s()",
		 this->function.c_str());

	log(MSG_DEBUG, this, fmt::sprintf("db query: %s", this->query));
}

/*
 * Send the query to the database and continue
 */
void Healthcheck_postgres::send_query() {
	assert(PQstatus(this->conn) == CONNECTION_OK);
	assert(!PQisBusy(this->conn));
	assert(PQisnonblocking(this->conn));

	/*
	 * PQsendQuery() can fail in non-blocking mode, and has to be
	 * re-tried.
	 */
	if (!PQsendQuery(this->conn, query))
		return this->register_io_event(EV_WRITE,
					       &Healthcheck_postgres::send_query);

	// The next step
	this->flush_query();
}

/*
 * Recursively flush the query on the socket and continue
 *
 * This is the part of libpq documentation we are implementing:
 *
 * > After sending any command or data on a nonblocking connection, call
 * > PQflush.  If it returns 1, wait for the socket to become read- or
 * > write-ready. If it becomes write-ready, call PQflush again.  If it
 * > becomes read-ready, call PQconsumeInput, then call PQflush again.
 * > Repeat until PQflush returns 0.  (It is necessary to check for
 * > read-ready and drain the input with PQconsumeInput, because
 * > the server can block trying to send us data, e.g. NOTICE messages,
 * > and won't read our data until we read its.)  Once PQflush returns 0,
 * > wait for the socket to be read-ready and then read the response as
 * > described above.
 */
void Healthcheck_postgres::flush_query() {
	assert(PQstatus(this->conn) == CONNECTION_OK);
	assert(PQisBusy(this->conn));
	assert(PQisnonblocking(this->conn));

	/*
	 * As the documentation says, we are calling PQconsumeInput()
	 * before, if the socket is read-ready.  Note that the purpose
	 * of this is not to consume the query results, but some other
	 * things, if they arrive.  We don't really expect other things,
	 * but it is better to be on the safer side.  We are going to
	 * consume the actual query result results on the next callback.
	 */
	if (this->event_flag == EV_READ)
		if (!PQconsumeInput(this->conn))
			return this->end_check(HC_FAIL, "cannot consume input");

	// We have to keep trying, until the socket is ready.
	if (PQflush(this->conn) != 0)
		return this->register_io_event(EV_READ | EV_WRITE,
					    &Healthcheck_postgres::flush_query);

	// The next step
	this->register_io_event(EV_READ, &Healthcheck_postgres::handle_query);
}

/*
 * Handle the query and continue
 *
 * This is the final step, before we call end_check() with successful
 * result type.  We are handling the query results we hopefully got
 * from the database.
 */
void Healthcheck_postgres::handle_query() {
	char *val;

	assert(this->event_flag == EV_READ);
	assert(PQstatus(this->conn) == CONNECTION_OK);
	assert(PQisnonblocking(this->conn));

	/*
	 * This is likely to be the place where it will fail, if
	 * the connection goes away, before we could make the query.
	 */
	if (!PQconsumeInput(this->conn))
		return this->end_check(HC_FAIL, "cannot consume db result");

	/*
	 * As the input is consumed, the connections must not be busy.
	 * If it would have been busy, PQgetResult() would block.
	 */
	assert(!PQisBusy(this->conn));

	this->query_result = PQgetResult(this->conn);

	if (PQresultStatus(this->query_result) != PGRES_TUPLES_OK)
		return this->end_check(HC_FAIL, "db result not ok");

	if (PQntuples(this->query_result) != 1)
		return this->end_check(HC_FAIL, "db result not 1 row");

	if (PQnfields(this->query_result) != 1)
		return this->end_check(HC_FAIL, "db result not 1 column");

	// 0 means the format is text which must always be the case.
	assert(PQfformat(this->query_result, 0) == 0);

	// Get the single cell
	val = PQgetvalue(this->query_result, 0, 0);

	if (strlen(val) != 1)
		return this->end_check(HC_FAIL, "db result not 1 char");

	if (val[0] != 't')
		return this->end_check(HC_FAIL, "db result false");

	return this->end_check(HC_PASS, "db result true");
}

/*
 * Override end_check() method to clean up things
 */
void Healthcheck_postgres::end_check(HealthcheckResult result, string message) {
	if (result != HC_PASS && this->conn != NULL) {
		char *error = PQerrorMessage(this->conn);

		if (error != NULL && strlen(error) > 0)
			log(MSG_CRIT, this, fmt::sprintf("db error: %s", error));

		if (verbose >= 2)
			log(MSG_DEBUG, this, fmt::sprintf("Last event %d after %d events",
				this->event_flag, this->event_counter));
	}

	if (this->query_result != NULL) {
		PQclear(this->query_result);
		this->query_result = NULL;
	}

	if (this->io_event != NULL) {
		event_del(this->io_event);
		event_free(this->io_event);
		this->io_event = NULL;
	}

	if (this->timeout_event != NULL) {
		event_del(this->timeout_event);
		event_free(this->timeout_event);
		this->timeout_event = NULL;
	}

	if (this->conn != NULL) {
		PQfinish(this->conn);
		this->conn = NULL;
	}

	Healthcheck::end_check(result, message);
}

/*
 * Helper method to register methods to libevent
 *
 * This function should not fail, but we cannot just continue if it does.
 * We have to set health check as failed, even though probably it has
 * nothing to do with the target server.
 */
void Healthcheck_postgres::register_io_event(short flag,
					     void (Healthcheck_postgres::*method)()) {

	// There are two events the caller can register.
	assert(!(flag & ~(EV_READ | EV_WRITE)));

	/*
	 * The check should fail with a timeout before this limit is
	 * reached.  It is useful at least for development to detect
	 * endless loops.  100 is a limit high enough to catch them,
	 * low enough to be hit before the timeout.
	 */
	if (this->event_counter++ > 100)
		return this->end_check(HC_PANIC, "too many events");

	this->callback_method = method;
	this->io_event = event_new(eventBase, PQsocket(this->conn), flag,
				   &Healthcheck_postgres::handle_io_event, this);

	if (this->io_event == NULL)
		return this->end_check(HC_PANIC, "cannot create event");

	// Note that we are registering it without a timeout.
	if (event_add(this->io_event, 0) != 0)
		return this->end_check(HC_PANIC, "cannot add event");
}

/*
 * Helper method to register the timeout event to libevent
 *
 * XXX This is a copy of the function above.  It should be shared by all
 * health checks.
 */
void Healthcheck_postgres::register_timeout_event() {

	/*
	 * We don't need t file descriptor or an event flag, because
	 * it will only be used for timeout.
	 */
	this->timeout_event =
		event_new(eventBase, -1, 0,
			  &Healthcheck_postgres::handle_timeout_event, this);

	if (this->timeout_event == NULL)
		return this->end_check(HC_PANIC, "cannot create event");

	if (event_add(this->timeout_event, &this->timeout) != 0)
		return this->end_check(HC_PANIC, "cannot add event");
}

/*
 * Static callback for I/O events
 *
 * This is a wrapper around the actual callback.  We are handling
 * the common errors, and working around the limitation that
 * the callback cannot be an object method.
 */
void Healthcheck_postgres::handle_io_event(int fd, short flag, void *arg) {
	Healthcheck_postgres *hc = (Healthcheck_postgres *) arg;

	/*
	 * We don't need the file descriptor, but as it is passed by
	 * libevent, lets check that it is the correct one.
	 */
	assert(PQsocket(hc->conn) == fd);

	/*
	 * We can only pass a single event flag to the callback method.
	 * There are other codes libevent could return, but we are not
	 * using them.
	 */
	assert(flag == EV_READ || flag == EV_WRITE);

	/*
	 * If the event happen while the check is not running, things went
	 * terribly wrong.
	 */
	assert(hc->is_running);

	// We are going to reuse this event.
	event_free(hc->io_event);
	hc->io_event = NULL;

	// Call the actual callback method
	hc->event_flag = flag;
	(hc->*(hc->callback_method))();
}

/*
 * Static callback for timeout events
 *
 * XXX This should be shared by all health checks.
 */
void Healthcheck_postgres::handle_timeout_event(int fd, short flag, void *arg) {
	Healthcheck_postgres *hc = (Healthcheck_postgres *) arg;

	assert(flag == EV_TIMEOUT);

	/*
	 * If the event happen while the check is not running, things went
	 * terribly wrong.
	 */
	assert(hc->is_running);

	hc->event_flag = flag;
	hc->end_check(HC_FAIL, "timeout");
}
