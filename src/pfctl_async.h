#ifndef _PFCTL_ASYNC_H_
#define _PFCTL_ASYNC_H_

#include <event2/event.h>
#include <functional>
#include <queue>
#include <string>
#include <vector>

using PfctlCallback =
    std::function<void(bool success, std::vector<std::string> output)>;

struct PfctlCommand {
  std::vector<std::string> args;
  PfctlCallback callback;
};

class PfctlAsync {
public:
  PfctlAsync(struct event_base *base);
  ~PfctlAsync();

  // Queue a pfctl command for async execution. Returns false if queue full.
  bool run_command(std::vector<std::string> args, PfctlCallback callback);

  // Run all queued commands synchronously (for shutdown).
  void drain();

  // Number of pending + running commands.
  size_t pending() const;

  // Is a command currently executing?
  bool busy() const;

private:
  void spawn_next();
  static void on_pipe_readable(evutil_socket_t fd, short events, void *arg);
  void collect_output();
  void handle_child_done();
  void try_reap_child();
  static void on_reap_timer(evutil_socket_t fd, short events, void *arg);

  struct event_base *base_;
  std::queue<PfctlCommand> queue_;

  // Currently running subprocess
  pid_t child_pid_ = -1;
  int pipe_fd_ = -1;
  struct event *pipe_event_ = nullptr;
  PfctlCallback current_callback_;
  std::string output_buffer_;
  struct event *reap_timer_ = nullptr;

  static const size_t MAX_QUEUE_LEN = 10;
};

#endif
