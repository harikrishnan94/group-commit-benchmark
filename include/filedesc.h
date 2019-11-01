#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <sys/types.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>
#include <utility>

class filedesc {
  int fd;

  filedesc(int a_fd) : fd(a_fd) {
    if (a_fd == -1) {
      throw std::system_error(errno, std::generic_category());
    }
  }

public:
  filedesc(const char *filepath, int flags)
      : filedesc(open(filepath, flags, S_IRUSR | S_IWUSR)) {}
  filedesc(filedesc &&o) : filedesc(std::exchange(o.fd, -1)) {}
  filedesc(const filedesc &) = delete;
  ~filedesc() {
    if (fd != -1) {
      close(fd);
    }
  }

  static filedesc open_in_temp(const char *filename, int flags) {
    const char *tmpdir = std::getenv("TMPDIR");
    const char pattern[] = "XXXXXXXX";
    tmpdir = tmpdir ? tmpdir : "/tmp/";
    size_t dirlen = std::strlen(tmpdir);
    size_t filename_len = std::strlen(filename);
    auto temp =
        std::make_unique<char[]>(dirlen + filename_len + sizeof(pattern) + 1);
    std::copy(tmpdir, tmpdir + dirlen, temp.get());
    std::copy(filename, filename + filename_len, temp.get() + dirlen);

    return filedesc{temp.get(), O_DSYNC | O_WRONLY | O_CREAT | O_EXCL};
  }

  int write(const struct iovec *vector, int count) {
    while (count > 0) {
      auto len = std::min(count, IOV_MAX);
      if (writev(fd, vector, len) == -1) {
        return -1;
      }

      vector += len;
      count -= IOV_MAX;
    }

    return 0;
  }

  int truncate(std::size_t newsize) { return ftruncate(fd, newsize); }
};