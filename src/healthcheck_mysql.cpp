//
// Testtool - MySQL Health Check
//
// Allow the MySQL/MariaDB connections to be load-balanced by checking
// the health of the database.
//
// Copyright (c) 2026 InnoGames GmbH

#define FMT_HEADER_ONLY

#include <cassert>
#include <cstring>
#include <errno.h>
#include <event2/event.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <mysql.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_mysql.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"

using namespace std;

extern struct event_base *eventBase;
extern int verbose;

/// The constructor
///
/// We are initialising the variables, only.  Nothing should be able
/// to fail in there.
Healthcheck_mysql::Healthcheck_mysql(const nlohmann::json &config,
                                     class LbNode *_parent_lbnode,
                                     string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {

  this->type = "mysql";

  this->port = safe_get<int>(config, "hc_port", 3306);
  // The port is passed on to an unsigned parameter, a negative value
  // would silently wrap to a huge port number.
  if (this->port < 1 || this->port > 65535)
    this->port = 3306;
  // This must stay a literal IP address.  mysql_real_connect_start()
  // resolves host names synchronously, which would block the single
  // event loop; passing an already-resolved address avoids that.
  this->host = *ip_address;
  this->dbname = safe_get<string>(config, "hc_dbname", "");
  this->user = safe_get<string>(config, "hc_user", "");
  this->password = safe_get<string>(config, "hc_password", "");
  this->query = safe_get<string>(config, "hc_query", "");

  // The password is intentionally kept out of the log prefix.
  this->log_prefix = fmt::sprintf("query: '%s' port: %d host: %s", this->query,
                                  this->port, this->host);
}

/// The entrypoint of the class
///
/// This starts the health check by choosing the first step.  After we
/// call the method for the first step, things will continue in
/// an asynchronous chain of events.
///
/// XXX Nobody checks the exit code of this function.  We must never
/// fail to call the first step for process to reach the end.
int Healthcheck_mysql::schedule_healthcheck(struct timespec *now) {

  // Perform general stuff for scheduled health check
  if (!Healthcheck::schedule_healthcheck(now))
    return false;

  this->event_counter = 0;
  this->event_flag = 0;
  this->conn = NULL;
  this->conn_ret = NULL;
  this->query_ret = 0;
  this->result = NULL;
  this->io_event = NULL;
  this->timeout_event = NULL;
  this->register_timeout_event();

  // The first step
  this->start_conn();

  return true;
}

/// Wait for whatever libmariadb asked for and continue
///
/// The non-blocking API returns a bit mask of the conditions it wants to
/// be woken up for: socket readability or writability, a socket
/// exception, and/or the expiry of its internal timeout whose duration
/// is available from mysql_get_timeout_value_ms().  All of them map onto
/// a single libevent event: exceptions surface as readability, and the
/// timeout is armed on the same event, so whichever condition occurs
/// first drives the next step.  The check's own timeout_event bounds the
/// whole check regardless of what is armed here.
///
/// This function should not fail, but we cannot just continue if it
/// does.  We have to set the health check as failed, even though it
/// probably has nothing to do with the target server.
void Healthcheck_mysql::register_step(int status,
                                      void (Healthcheck_mysql::*method)()) {
  short flag = 0;

  if (status & MYSQL_WAIT_READ)
    flag |= EV_READ;
  if (status & MYSQL_WAIT_WRITE)
    flag |= EV_WRITE;
  if (status & MYSQL_WAIT_EXCEPT)
    flag |= EV_READ;

  struct timeval tv;
  struct timeval *tvp = NULL;
  if (status & MYSQL_WAIT_TIMEOUT) {
    unsigned int ms = mysql_get_timeout_value_ms(this->conn);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    tvp = &tv;
  }

  // The check should fail with a timeout before this limit is
  // reached.  It is useful at least for development to detect
  // endless loops.  100 is a limit high enough to catch them,
  // low enough to be hit before the timeout.  Only this single check
  // is failed: a misbehaving backend must not take down health
  // checking for every other pool in this single-process daemon.
  if (this->event_counter++ > 100)
    return this->end_check(HealthcheckResult::HC_FAIL, "too many events");

  this->callback_method = method;
  this->io_event =
      event_new(eventBase, flag ? mysql_get_socket(this->conn) : -1, flag,
                &Healthcheck_mysql::handle_io_event, this);

  if (this->io_event == NULL)
    return this->end_check(HealthcheckResult::HC_PANIC, "cannot create event");

  if (event_add(this->io_event, tvp) != 0)
    return this->end_check(HealthcheckResult::HC_PANIC, "cannot add event");
}

/// Translate the fired libevent flag into a libmariadb wait status
///
/// This is fed into the _cont() functions so that libmariadb knows which
/// conditions actually occurred.  A fired libevent timer means the wait
/// libmariadb asked for via MYSQL_WAIT_TIMEOUT has elapsed.
int Healthcheck_mysql::event_flag_to_wait_status() {
  int status = 0;

  if (this->event_flag & EV_READ)
    status |= MYSQL_WAIT_READ;
  if (this->event_flag & EV_WRITE)
    status |= MYSQL_WAIT_WRITE;
  if (this->event_flag & EV_TIMEOUT)
    status |= MYSQL_WAIT_TIMEOUT;

  return status;
}

/// Start a non-blocking connection and continue
///
/// mysql_real_connect_start() begins the connection and returns a bit mask
/// of the socket events it needs to wait for, or 0 when the connection has
/// already completed and no waiting is needed.
void Healthcheck_mysql::start_conn() {
  this->conn = mysql_init(NULL);

  // If it is NULL, the memory allocation must have been failed.
  if (this->conn == NULL)
    return this->end_check(HealthcheckResult::HC_PANIC,
                           "cannot init db connection");

  // The non-blocking mode is what lets us drive the whole check from
  // libevent without ever blocking the single testtool thread.  It has
  // to be set before the connection is started.
  if (mysql_options(this->conn, MYSQL_OPT_NONBLOCK, 0))
    return this->end_check(HealthcheckResult::HC_PANIC,
                           "cannot non-block db connection");

  // Bound every network read and write on this connection, including
  // the synchronous COM_QUIT that mysql_close() sends on teardown.
  // Without these bounds the library polls without a timeout on paths
  // outside its non-blocking machinery, which could freeze the event
  // loop against a peer that stopped reading.  The check timeout is
  // rounded up to the API's granularity of full seconds.
  unsigned int rw_timeout = (this->timeout_to_ms() + 999) / 1000;
  if (rw_timeout < 1)
    rw_timeout = 1;
  if (mysql_options(this->conn, MYSQL_OPT_READ_TIMEOUT, &rw_timeout) ||
      mysql_options(this->conn, MYSQL_OPT_WRITE_TIMEOUT, &rw_timeout))
    return this->end_check(HealthcheckResult::HC_PANIC,
                           "cannot set db timeouts");

  // Always require an encrypted connection.  This only enforces that
  // the transport is encrypted; the server certificate is not verified.
  my_bool ssl_enforce = 1;
  if (mysql_options(this->conn, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce))
    return this->end_check(HealthcheckResult::HC_PANIC,
                           "cannot enforce db tls");

  // Empty strings are turned into NULL so that libmariadb applies its
  // own defaults (e.g. no default database) instead of trying to use
  // the empty value literally.
  const char *user_c = this->user.empty() ? NULL : this->user.c_str();
  const char *pass_c = this->password.empty() ? NULL : this->password.c_str();
  const char *db_c = this->dbname.empty() ? NULL : this->dbname.c_str();

  int status =
      mysql_real_connect_start(&this->conn_ret, this->conn, this->host.c_str(),
                               user_c, pass_c, db_c, this->port, NULL, 0);

  // The connection can finish right away, e.g. with a local refusal.
  if (status == 0)
    return this->finish_conn();

  // The next step
  this->register_step(status, &Healthcheck_mysql::poll_conn);
}

/// Continue a non-blocking connection and continue
///
/// Once the events start_conn() asked for have occurred,
/// mysql_real_connect_cont() is called with the events that actually fired.
/// It returns a new wait mask, or 0 when the connection has completed.
void Healthcheck_mysql::poll_conn() {
  int status = mysql_real_connect_cont(&this->conn_ret, this->conn,
                                       this->event_flag_to_wait_status());

  if (status == 0)
    return this->finish_conn();

  // The next step, again
  this->register_step(status, &Healthcheck_mysql::poll_conn);
}

/// Check the connection result and continue
///
/// The out-param is NULL when the connection could not be established,
/// e.g. because of a refusal or an authentication failure.
void Healthcheck_mysql::finish_conn() {
  if (this->conn_ret == NULL)
    return this->end_check(HealthcheckResult::HC_FAIL, "db connection failed");

  // The next step
  this->send_query();
}

/// Start sending the query and continue
void Healthcheck_mysql::send_query() {
  int status = mysql_real_query_start(
      &this->query_ret, this->conn, this->query.c_str(), this->query.length());

  if (status == 0)
    return this->finish_query();

  // The next step
  this->register_step(status, &Healthcheck_mysql::poll_query);
}

/// Continue sending the query and continue
void Healthcheck_mysql::poll_query() {
  int status = mysql_real_query_cont(&this->query_ret, this->conn,
                                     this->event_flag_to_wait_status());

  if (status == 0)
    return this->finish_query();

  // The next step, again
  this->register_step(status, &Healthcheck_mysql::poll_query);
}

/// Check the query result and continue
///
/// The out-param is the return value of mysql_real_query(): zero on
/// success, non-zero on any failure to run the given query.
void Healthcheck_mysql::finish_query() {
  if (this->query_ret != 0)
    return this->end_check(HealthcheckResult::HC_FAIL, "db query failed");

  // The next step
  this->store_result();
}

/// Start reading the whole result set and continue
///
/// We deliberately use mysql_store_result() and not mysql_use_result():
/// the former buffers the entire result client-side, so that the later
/// mysql_fetch_row() and mysql_free_result() calls do no network I/O and
/// therefore cannot block the event loop.
void Healthcheck_mysql::store_result() {
  int status = mysql_store_result_start(&this->result, this->conn);

  if (status == 0)
    return this->handle_result();

  // The next step
  this->register_step(status, &Healthcheck_mysql::poll_store);
}

/// Continue reading the whole result set and continue
void Healthcheck_mysql::poll_store() {
  int status = mysql_store_result_cont(&this->result, this->conn,
                                       this->event_flag_to_wait_status());

  if (status == 0)
    return this->handle_result();

  // The next step, again
  this->register_step(status, &Healthcheck_mysql::poll_store);
}

/// Handle the result and continue
///
/// This is the final step, before we call end_check() with a successful
/// result type.  We are handling the query results we hopefully got from
/// the database.  A truthy value in MySQL is rendered as the text "1".
void Healthcheck_mysql::handle_result() {
  // A NULL result means either an error, or a statement that produced
  // no result set at all.  Either way it is not the single truthy row
  // we require.
  if (this->result == NULL)
    return this->end_check(HealthcheckResult::HC_FAIL, "db result not ok");

  if (mysql_num_rows(this->result) != 1)
    return this->end_check(HealthcheckResult::HC_FAIL, "db result not 1 row");

  if (mysql_num_fields(this->result) != 1)
    return this->end_check(HealthcheckResult::HC_FAIL,
                           "db result not 1 column");

  // The result set is fully buffered client-side by mysql_store_result(),
  // so this does not do any network I/O and cannot block.
  MYSQL_ROW row = mysql_fetch_row(this->result);

  if (row == NULL || row[0] == NULL)
    return this->end_check(HealthcheckResult::HC_FAIL, "db result empty");

  if (strcmp(row[0], "1") != 0)
    return this->end_check(HealthcheckResult::HC_FAIL, "db result false");

  return this->end_check(HealthcheckResult::HC_PASS, "db result true");
}

/// Override end_check() method to clean up things
void Healthcheck_mysql::end_check(HealthcheckResult result, string message) {
  if (result != HealthcheckResult::HC_PASS && this->conn != NULL) {
    const char *error = mysql_error(this->conn);

    if (error != NULL && strlen(error) > 0)
      message += fmt::sprintf(" db error: %s", error);

    if (verbose >= 2)
      message += fmt::sprintf(" Last event flag 0x%02x after %d events",
                              this->event_flag, this->event_counter);
  }

  if (this->result != NULL) {
    mysql_free_result(this->result);
    this->result = NULL;
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
    // mysql_close() sends a best-effort COM_QUIT over the library's
    // synchronous path, which polls the socket without regard to the
    // non-blocking mode.  That poll is bounded by the read and write
    // timeouts set in start_conn(); without them it could stall the
    // event loop indefinitely against a peer that stopped reading.
    mysql_close(this->conn);
    this->conn = NULL;
  }

  Healthcheck::end_check(result, message);
}

/// Helper method to register the timeout event to libevent
///
/// XXX The timeout event setup is duplicated across the health check types
/// and should be shared.
void Healthcheck_mysql::register_timeout_event() {

  // We don't need a file descriptor or an event flag, because
  // it will only be used for timeout.
  this->timeout_event = event_new(
      eventBase, -1, 0, &Healthcheck_mysql::handle_timeout_event, this);

  if (this->timeout_event == NULL)
    return this->end_check(HealthcheckResult::HC_PANIC, "cannot create event");

  if (event_add(this->timeout_event, &this->timeout) != 0)
    return this->end_check(HealthcheckResult::HC_PANIC, "cannot add event");
}

/// Static callback for I/O events
///
/// This is a wrapper around the actual callback.  We are handling
/// the common errors, and working around the limitation that
/// the callback cannot be an object method.
void Healthcheck_mysql::handle_io_event(int fd, short flag, void *arg) {
  // Make compiler happy.  The fd is only used by the assert() below,
  // which is compiled out in release builds.
  (void)(fd);

  Healthcheck_mysql *hc = (Healthcheck_mysql *)arg;

  // For a socket wait we don't need the file descriptor, but as it is
  // passed by libevent, lets check that it is the correct one.  A pure
  // timed wait uses fd -1 and fires EV_TIMEOUT instead.
  if (flag & (EV_READ | EV_WRITE))
    assert(mysql_get_socket(hc->conn) == fd);

  // libmariadb may ask us to wait for readability and writability at
  // the same time, so both flags together are valid too; a timed wait
  // arrives as EV_TIMEOUT.
  assert(flag & (EV_READ | EV_WRITE | EV_TIMEOUT));

  // If the event happen while the check is not running, things went
  // terribly wrong.
  assert(hc->is_running);

  // We are going to reuse this event.
  event_free(hc->io_event);
  hc->io_event = NULL;

  // Call the actual callback method
  hc->event_flag = flag;
  (hc->*(hc->callback_method))();
}

/// Static callback for timeout events
///
/// XXX This callback is duplicated across the health check types and should
/// be shared.
void Healthcheck_mysql::handle_timeout_event(int fd, short flag, void *arg) {
  // Make compiler happy
  (void)(fd);

  Healthcheck_mysql *hc = (Healthcheck_mysql *)arg;

  assert(flag == EV_TIMEOUT);

  // If the event happen while the check is not running, things went
  // terribly wrong.
  assert(hc->is_running);

  hc->event_flag = flag;
  hc->end_check(HealthcheckResult::HC_FAIL, "timeout");
}
