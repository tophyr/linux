#pragma once

#include <expected>

namespace std {
inline std::ostream& operator<<(std::ostream& out, std::monostate const&) {
  return out << "monostate{}";
}
template <typename V>
inline std::ostream& operator<<(std::ostream& out, std::expected<V, int> const& exp) {
  if (exp.has_value()) {
    return out << "Value: " << exp.value();
  } else {
    return out << "Error: " << exp.error() << " (" << strerror(exp.error()) << ")";
  }
}
}

namespace {

enum interrupt_tolerance_t : bool {
  interrupt_intolerant = false,
  interrupt_tolerant = true,
};
template <typename Callable, typename... Args>
inline auto errno_check(interrupt_tolerance_t interrupt_tolerance, Callable&& callable, Args&&... args) {
  errno = 0;
  do {
    auto ret = std::forward<Callable>(callable)(std::forward<Args>(args)...);
    if (ret < 0) {
      if (errno == EINTR && static_cast<bool>(interrupt_tolerance)) {
        continue;
      }
      throw std::system_error(errno, std::generic_category());
    }
    return ret;
  } while (true);
}
template <typename Callable, typename... Args>
inline auto errno_check(Callable&& callable, Args&&... args) {
  return errno_check(interrupt_intolerant, std::forward<Callable>(callable), std::forward<Args>(args)...);
}

struct signal_handler {
  using sa_handler_t = void(int);
  using sa_sigaction_t = void(int, siginfo_t*, void*);

  signal_handler(int signum, sa_handler_t* handler)
    : signum_{signum} {
    struct sigaction sa{};
    sa.sa_handler = handler,
    errno_check(sigaction, signum_, &sa, &old_);
  }

  ~signal_handler() noexcept(false) {
    errno_check(sigaction, signum_, &old_, nullptr);
  }

 private:
  int signum_;
  struct sigaction old_;
};

struct unique_fd {
  explicit unique_fd(int fd)
    : fd_{fd} {
    enforce_validity();
  }
  unique_fd(unique_fd const&) = delete;
  unique_fd(unique_fd&& other)
    : unique_fd{other.release()} {}
  unique_fd& operator=(unique_fd const&) = delete;
  unique_fd& operator=(unique_fd&& other) {
    if (this != &other) {
      this->~unique_fd();
      new (this) unique_fd{std::move(other)};
    }
    return *this;
  }
  ~unique_fd() noexcept(false) {
    if (valid()) {
      close();
    }
  }

  bool valid() const {
    return fd_ >= 0;
  }

  int fd() const {
    enforce_validity();
    return fd_;
  }

  operator int() const {
    return fd();
  }

  int release() {
    enforce_validity();
    return std::exchange(fd_, -1);
  }

  void close() {
    errno_check(::close, release());
  }

 private:
  void enforce_validity() const {
    if (!valid()) {
      throw std::runtime_error("invalid file descriptor");
    }
  }
  int fd_;
};

static constexpr size_t kDefaultUniqueFdReadMax{65535};
inline std::string read(unique_fd const& fd, size_t size = kDefaultUniqueFdReadMax) {
  std::string buf;
  buf.resize_and_overwrite(size, [&fd](char* p, size_t sz) { return errno_check(::read, fd, p, sz); });
  return buf;
}

inline auto write(unique_fd const& fd, std::string_view buf) {
  return errno_check(::write, fd, buf.data(), buf.size());
}

struct unique_pipe {
  unique_pipe()
    : unique_pipe{create()} {}

  unique_fd read;
  unique_fd write;

 private:
  unique_pipe(unique_fd read, unique_fd write)
    : read{std::move(read)}, write{std::move(write)} {}

  static unique_pipe create() {
    int fds[2];
    errno_check(pipe, fds);
    return unique_pipe{unique_fd{fds[0]}, unique_fd{fds[1]}};
  }
};

struct proxycontrol {
  explicit proxycontrol(unique_fd const& proxyfd)
    : controlfd_{get_control_fd(proxyfd)} {}

  void set_link(unique_fd const& link) {
    set_link_raw(link.fd());
  }
  
  void set_link_raw(int link) {
    errno_check(ioctl, controlfd_, PROXY_IOCTL_SET_LINK, link);
  }

  void clear_link() {
    errno_check(ioctl, controlfd_, PROXY_IOCTL_CLEAR_LINK);
  }

  unique_fd& fd() {
    return controlfd_;
  }

 private:
  static unique_fd get_control_fd(unique_fd const& proxyfd) {
    int controlfd = -1;
    errno_check(ioctl, proxyfd, PROXY_IOCTL_GET_CONTROL_FD, &controlfd);
    return unique_fd{controlfd};
  }

  unique_fd controlfd_;
};
std::ostream& operator<<(std::ostream& out, proxycontrol const&) {
  return out << "{proxycontrol}";
}

#define EXPECT_EXPECTED_ANY(test_expr) do {                         \
  auto&& test_value = test_expr;                                    \
  ASSERT_TRUE(test_value.has_value()) << test_value;                \
} while(false)
#define EXPECT_EXPECTED(test_expr, expected_expr) do {              \
  auto&& test_value = test_expr;                                    \
  auto&& expected_value = expected_expr;                            \
  ASSERT_TRUE(test_value.has_value()) << test_value;                \
  EXPECT_TRUE(test_value.value() == expected_value) << test_value;  \
} while (false)
#define EXPECT_UNEXPECTED_ANY(test_expr) do {                       \
  auto&& test_value = test_expr;                                    \
  ASSERT_FALSE(test_value.has_value()) << test_value;               \
} while(false)
#define EXPECT_UNEXPECTED(test_expr, expected_expr) do {            \
  auto&& test_value = test_expr;                                    \
  auto&& expected_err = expected_expr;                              \
  ASSERT_FALSE(test_value.has_value()) << test_value;               \
  EXPECT_EQ(test_value.error(), expected_err) << test_value;        \
} while (false)

#define EXPECT_ERRNO(test_expr, expected_errno) do {                              \
  try {                                                                           \
    test_expr;                                                                    \
    ADD_FAILURE() << "Expected failure " << expected_errno << " but succeeded";   \
  } catch (std::system_error const& se) {                                         \
    if (se.code() != std::error_code(expected_errno, std::generic_category())) {  \
      std::cerr << "got system_error but wrong code! expected " << expected_errno << " but is " << se.code() << std::endl; \
      throw;                                                                      \
    }                                                                             \
  }                                                                               \
} while(false)

static constexpr auto kSuccess{"success"sv};
static constexpr auto kReady{"ready"sv};
static constexpr uid_t kDroppedUid{1};
template <typename Parent, typename Child>
static void forktest(Parent&& parent, Child&& child) {
  unique_pipe to_parent;
  unique_pipe to_child;

  if (auto child_pid = fork()) {
    to_parent.write.close();
    to_child.read.close();

    std::forward<Parent>(parent)(to_parent.read, to_child.write);
    write(to_child.write, kReady);

    errno_check(interrupt_tolerant, waitpid, child_pid, nullptr, 0);
    EXPECT_EQ(read(to_parent.read), kSuccess);
  } else {
    try {
      to_parent.read.close();
      to_child.write.close();
      errno_check(close, STDOUT_FILENO);
      errno_check(close, STDERR_FILENO);
      errno_check(close, STDIN_FILENO);

      setuid(kDroppedUid);
      std::forward<Child>(child)(to_child.read, to_parent.write);
      EXPECT_EQ(read(to_child.read), kReady);

      if (::testing::UnitTest::GetInstance()->current_test_info()->result()->Passed()) {
        write(to_parent.write, kSuccess);
      } else {
        write(to_parent.write, "child has failures or skips");
      }
    } catch (std::exception const& e) {
      write(to_parent.write, "Exception in child: ");
      write(to_parent.write, e.what());
    }
    _exit(0);
  }
}

} // namespace (anonymous)
