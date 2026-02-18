#include "echo_test.h"
#include "lsd_pty.h"
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <atomic>

std::atomic<bool> got_output = false;

void read_callback(const char* msg, size_t size)
{
  if (std::strstr(msg, "test"))
    got_output = true;
}

void echo_test()
{
  PTY pty;
  pty.setReadCallback(read_callback);
  pty.spawn();
  pty.write("clear\n",7);
  pty.write("echo test\n", 10);

  while (!got_output)
    usleep(1000);
}





int main()
{
  echo_test();
}

