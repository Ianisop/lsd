#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

class TTY
{
private:
  int tty_fd;
  struct termios original_termios;

public:
  TTY() : tty_fd(-1)
  {
  }

  ~TTY()
  {
    if(tty_fd != -1)
      {
        // Restore original terminal settings
        tcsetattr(tty_fd, TCSANOW, &original_termios);
        close(tty_fd);
      }
  }

  bool openTTY(const char *tty_path)
  {
    // Open the TTY
    tty_fd = open(tty_path, O_RDWR | O_NOCTTY);
    if(tty_fd == -1)
      {
        std::cerr << "Failed to open TTY " << tty_path << ": "
                  << strerror(errno) << std::endl;
        return false;
      }

    // Save current terminal settings
    if(tcgetattr(tty_fd, &original_termios) == -1)
      {
        std::cerr << "Failed to get terminal attributes: "
                  << strerror(errno) << std::endl;
        return false;
      }

    return true;
  }

  bool setRawMode()
  {
    struct termios raw = original_termios;

    // Set raw mode (similar to cfmakeraw)
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                     | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;

    // Set minimum read and timeout
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if(tcsetattr(tty_fd, TCSAFLUSH, &raw) == -1)
      {
        std::cerr << "Failed to set raw mode: " << strerror(errno) << std::endl;
        return false;
      }

    return true;
  }

  // Read from TTY
  std::string readFromTTY()
  {
    char buffer[256];
    ssize_t bytes = read(tty_fd, buffer, sizeof(buffer) - 1);
    if(bytes > 0)
      {
        buffer[bytes] = '\0';
        return std::string(buffer);
      }
    return "";
  }

  // Write to TTY
  bool writeToTTY(const std::string &data)
  {
    ssize_t bytes = write(tty_fd, data.c_str(), data.length());
    if(bytes < 0)
      {
        std::cerr << "Failed to write to TTY: " << strerror(errno) << std::endl;
        return false;
      }
    return true;
  }
};
