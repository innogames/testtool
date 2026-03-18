#include "pfctl_async.h"
#include "msg.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <event2/util.h>

extern std::string pfctl_command;
extern bool pf_action;

PfctlAsync::PfctlAsync(struct event_base *base)
    : base_(base)
{
}

PfctlAsync::~PfctlAsync()
{
    if (!queue_.empty() || child_pid_ != -1) {
        drain();
    }
    if (pipe_event_ != nullptr) {
        event_del(pipe_event_);
        event_free(pipe_event_);
        pipe_event_ = nullptr;
    }
}

bool PfctlAsync::run_command(std::vector<std::string> args, PfctlCallback callback)
{
    if (queue_.size() >= MAX_QUEUE_LEN && child_pid_ != -1) {
        return false;
    }

    if (!pf_action) {
        callback(true, {});
        return true;
    }

    queue_.push(PfctlCommand{std::move(args), std::move(callback)});

    if (child_pid_ == -1) {
        spawn_next();
    }

    return true;
}

void PfctlAsync::spawn_next()
{
    if (queue_.empty()) {
        return;
    }

    PfctlCommand cmd = std::move(queue_.front());
    queue_.pop();
    current_callback_ = std::move(cmd.callback);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        log(MessageType::MSG_CRIT, "PfctlAsync: pipe() failed: " + std::string(strerror(errno)));
        auto cb = std::move(current_callback_);
        current_callback_ = nullptr;
        cb(false, {});
        if (child_pid_ == -1) spawn_next();
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log(MessageType::MSG_CRIT, "PfctlAsync: fork() failed: " + std::string(strerror(errno)));
        close(pipefd[0]);
        close(pipefd[1]);
        auto cb = std::move(current_callback_);
        current_callback_ = nullptr;
        cb(false, {});
        if (child_pid_ == -1) spawn_next();
        return;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Build argv: pfctl_command, "-q", then each arg, null-terminated
        std::vector<const char *> argv;
        argv.push_back(pfctl_command.c_str());
        argv.push_back("-q");
        for (const auto &arg : cmd.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(pfctl_command.c_str(), const_cast<char *const *>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);
    child_pid_ = pid;
    pipe_fd_ = pipefd[0];
    evutil_make_socket_nonblocking(pipe_fd_);
    output_buffer_.clear();
    pipe_event_ = event_new(base_, pipe_fd_, EV_READ | EV_PERSIST, on_pipe_readable, this);
    event_add(pipe_event_, NULL);
}

void PfctlAsync::on_pipe_readable(evutil_socket_t fd, short events, void *arg)
{
    PfctlAsync *self = static_cast<PfctlAsync *>(arg);
    self->collect_output();
}

void PfctlAsync::collect_output()
{
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd_, buf, sizeof(buf))) > 0) {
        output_buffer_.append(buf, n);
    }
    if (n == 0) {
        handle_child_done();
    }
    // n < 0 && errno == EAGAIN: return, more data later
}

void PfctlAsync::handle_child_done()
{
    event_del(pipe_event_);
    event_free(pipe_event_);
    pipe_event_ = nullptr;

    close(pipe_fd_);
    pipe_fd_ = -1;

    int status;
    waitpid(child_pid_, &status, 0);
    child_pid_ = -1;

    bool success = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Parse output_buffer_ into lines
    std::vector<std::string> lines;
    std::istringstream stream(output_buffer_);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }

    // Move callback to local before invoking — the callback may call
    // run_command() which triggers spawn_next() and overwrites current_callback_.
    auto cb = std::move(current_callback_);
    current_callback_ = nullptr;

    cb(success, lines);

    // Only spawn next if the callback didn't already do it via run_command().
    if (child_pid_ == -1) {
        spawn_next();
    }
}

void PfctlAsync::drain()
{
    // If a child is currently running, finish it synchronously
    if (child_pid_ != -1) {
        if (pipe_event_ != nullptr) {
            event_del(pipe_event_);
            event_free(pipe_event_);
            pipe_event_ = nullptr;
        }

        // Read remaining pipe data synchronously
        char buf[4096];
        ssize_t n;
        while ((n = read(pipe_fd_, buf, sizeof(buf))) > 0) {
            output_buffer_.append(buf, n);
        }
        close(pipe_fd_);
        pipe_fd_ = -1;

        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;

        bool success = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

        std::vector<std::string> lines;
        std::istringstream stream(output_buffer_);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
        }

        if (current_callback_) {
            current_callback_(success, lines);
            current_callback_ = nullptr;
        }
    }

    // Process remaining queued commands synchronously
    while (!queue_.empty()) {
        PfctlCommand cmd = std::move(queue_.front());
        queue_.pop();

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            log(MessageType::MSG_CRIT, "PfctlAsync::drain: pipe() failed: " + std::string(strerror(errno)));
            cmd.callback(false, {});
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            log(MessageType::MSG_CRIT, "PfctlAsync::drain: fork() failed: " + std::string(strerror(errno)));
            close(pipefd[0]);
            close(pipefd[1]);
            cmd.callback(false, {});
            continue;
        }

        if (pid == 0) {
            // Child process
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            std::vector<const char *> argv;
            argv.push_back(pfctl_command.c_str());
            argv.push_back("-q");
            for (const auto &arg : cmd.args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);

            execvp(pfctl_command.c_str(), const_cast<char *const *>(argv.data()));
            _exit(127);
        }

        // Parent: blocking read and wait
        close(pipefd[1]);
        std::string out_buf;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            out_buf.append(buf, n);
        }
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);

        bool success = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

        std::vector<std::string> lines;
        std::istringstream stream(out_buf);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
        }

        cmd.callback(success, lines);
    }
}

size_t PfctlAsync::pending() const
{
    return queue_.size() + (child_pid_ != -1 ? 1 : 0);
}

bool PfctlAsync::busy() const
{
    return child_pid_ != -1;
}
