//
// Testtool - Logging Routines
//
// Copyright (c) 2019 InnoGames GmbH
//

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/trivial.hpp>
#include <boost/shared_ptr.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <string>
#include <syslog.h>

#include "lb_pool.h"
#include "msg.h"

using namespace std;
using namespace boost;

namespace logging = boost::log;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;

src::severity_logger<msgType> slg;
typedef sinks::synchronous_sink<sinks::text_file_backend> file_sink;

void init_file_collecting(boost::shared_ptr<file_sink> sink) {
  sink->locked_backend()->set_file_collector(sinks::file::make_collector(
      keywords::target = "/var/do_log/iglb/", keywords::max_files = 10));
}

void init_logging() {
  boost::shared_ptr<file_sink> sink(
      new file_sink(keywords::file_name = "testtool-%Y%m%d_%H%M%S.log",
                    keywords::rotation_size = 10 * 1024 * 1024));

  init_file_collecting(sink);
  sink->locked_backend()->scan_for_files();

  /*
    sink->set_formatter(expr::format("%1% %2% %3%") %
                            expr::attr<boost::posix_time::ptime>("TimeStamp") %
                            logging::trivial::severity &
                        expr::xml_decor[expr::stream << expr::smessage]);
                        */

  boost::shared_ptr<logging::core> core = logging::core::get();

  logging::core::get()->add_sink(sink);
}

void do_log(msgType loglevel, string msg) {
  cout << msg << endl;
  BOOST_LOG_SEV(slg, loglevel) << msg;
}

void do_log(msgType loglevel, LbPool *lbpool, string msg) {
  string out = "lbpool: " + lbpool->name + " " + msg;
  do_log(loglevel, out);
}

void do_log(msgType loglevel, LbNode *lbnode, string msg) {
  string out = "lbnode: " + lbnode->name + " " + msg;
  do_log(loglevel, lbnode->parent_lbpool, out);
}

void do_log(msgType loglevel, Healthcheck *hc, string msg) {
  string out = "healthcheck: " + hc->type + " " + hc->log_prefix + " " + msg;
  do_log(loglevel, hc->parent_lbnode, out);
}
