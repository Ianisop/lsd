#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

class PTY
{
public:
  using ReadCallback = std::function<void(const char *, size_t)>;

  PTY() = default;
  ~PTY() { stop(); }

  bool spawn(int cols = 80, int rows = 24, const char *shell = "/bin/bash")
  {
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if(master_fd < 0) return false;
    if(grantpt(master_fd) < 0) return false;
    if(unlockpt(master_fd) < 0) return false;
    char *name = ptsname(master_fd);
    if(!name) return false;
    slave_name = name;

    // Set size before fork so child inherits correct $COLUMNS/$LINES
    struct winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(master_fd, TIOCSWINSZ, &ws);

    child_pid = fork();
    if(child_pid < 0) return false;
    if(child_pid == 0)
      {
        setsid();
        int sfd = open(slave_name.c_str(), O_RDWR);
        if(sfd < 0) _exit(1);
        ioctl(sfd, TIOCSCTTY, 0);
        dup2(sfd, 0);
        dup2(sfd, 1);
        dup2(sfd, 2);
        if(sfd > 2) close(sfd);
        close(master_fd);
        // Do NOT modify termios/ECHO here — the original bug that made
        // typing invisible. Kernel line discipline handles echo in cooked
        // mode; raw-mode programs manage it themselves.
        setenv("TERM", "xterm-256color", 1);
        execlp(shell, shell, nullptr);
        _exit(1);
      }
    running = true;
    reader = std::thread(&PTY::readerLoop, this);
    return true;
  }

  void setReadCallback(ReadCallback cb) { callback = std::move(cb); }

  bool write(const char *data, size_t len)
  {
    if(master_fd < 0) return false;
    return ::write(master_fd, data, len) == (ssize_t)len;
  }

  void resize(int cols, int rows)
  {
    if(master_fd < 0) return;
    struct winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(master_fd, TIOCSWINSZ, &ws);
    // Kernel automatically sends SIGWINCH to foreground process group
  }

  int fd() const { return master_fd; }

  void stop()
  {
    running = false;
    if(master_fd >= 0)
      {
        int t = master_fd;
        master_fd = -1;
        close(t);
      }
    if(reader.joinable()) reader.join();
    if(child_pid > 0)
      {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
        child_pid = -1;
      }
  }

private:
  int master_fd = -1;
  pid_t child_pid = -1;
  std::string slave_name;
  std::atomic<bool> running{false};
  std::thread reader;
  ReadCallback callback;

  void readerLoop()
  {
    std::vector<char> buf(4096);
    while(running)
      {
        fd_set fds;
        FD_ZERO(&fds);
        int fd = master_fd;
        if(fd < 0) break;
        FD_SET(fd, &fds);
        struct timeval tv{};
        tv.tv_usec = 50000;
        int r = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if(r < 0) break;
        if(r == 0) continue;
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if(n > 0)
          {
            if(callback) callback(buf.data(), (size_t)n);
          }
        else
          break;
      }
  }
};