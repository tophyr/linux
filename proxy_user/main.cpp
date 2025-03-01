#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/reboot.h>
#include <string_view>
#include <thread>
#include <asm/termbits.h>
#include <sys/ioctl.h>

#include <gtest/gtest.h>

using namespace std::literals;

#define PROXY_MAGIC 0xC5
#define PROXY_IOCTL_GET_CONTROL_FD _IOR(PROXY_MAGIC, 0, int)
#define PROXY_IOCTL_SET_LINK _IOW(PROXY_MAGIC, 1, int)
#define PROXY_IOCTL_CLEAR_LINK _IO(PROXY_MAGIC, 2)

#include "helpers.h"

TEST(Standalone, OpenAndClose) {
  int fd = open("/dev/proxy", O_RDONLY);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(close(fd), 0) << strerror(errno);
}

TEST(Standalone, InterleavedProxyControl) {
  std::optional<unique_fd> proxy{open("/dev/proxy", O_RDONLY)};
  proxycontrol control{*proxy};
  proxy.reset();
}

struct ProxyTest : testing::Test {
  ProxyTest()
    : proxy_{errno_check(open, "/dev/proxy", O_RDONLY)} {}

 protected:
  unique_fd proxy_;
};

TEST_F(ProxyTest, UnlinkedReadFailsNotConn) {
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
}

TEST_F(ProxyTest, UnlinkedNonProxyIoctlFailsNotConn) {
  EXPECT_ERRNO(errno_check(ioctl, proxy_.fd(), 4), ENOTCONN);
}

TEST_F(ProxyTest, UnlinkedProxySetLinkIoctlFailsNotConn) {
  EXPECT_ERRNO(errno_check(ioctl, proxy_.fd(), PROXY_IOCTL_SET_LINK, 42), ENOTCONN);
}

TEST_F(ProxyTest, UnlinkedProxyClearLinkIoctlFailsNotConn) {
  EXPECT_ERRNO(errno_check(ioctl, proxy_.fd(), PROXY_IOCTL_CLEAR_LINK), ENOTCONN);
}

struct ControlledProxyTest : ProxyTest {
  ControlledProxyTest()
    : control_{proxy_} {}

 protected:
  proxycontrol control_;
};

TEST_F(ControlledProxyTest, NothingJustGetControl) {}

TEST_F(ControlledProxyTest, CloseAndReopenControl) {
  control_.fd().close();
  control_ = proxycontrol{proxy_};
}

TEST_F(ControlledProxyTest, GetControlFdTwoAtOnce) {
  proxycontrol{proxy_};
}

TEST_F(ControlledProxyTest, GetControlFdInterleaved) {
  control_ = proxycontrol{proxy_};
}

TEST_F(ControlledProxyTest, IsStillUnlinkedAfterGettingControl) {
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
}

TEST_F(ControlledProxyTest, ClearUnlinkedProxyIsStillUnlinked) {
  control_.clear_link();
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
}

TEST_F(ControlledProxyTest, LinkBadFdFails) {
  EXPECT_ERRNO(control_.set_link_raw(42), EBADF);
}

TEST_F(ControlledProxyTest, LinkToSelfFails) {
  EXPECT_ERRNO(control_.set_link(proxy_), EBADF);
}

TEST_F(ControlledProxyTest, LinkToControlFails) {
  EXPECT_ERRNO(control_.set_link(control_.fd()), EBADF);
}

struct LinkedPipeReadProxyTest : ControlledProxyTest {
  LinkedPipeReadProxyTest() {
    control_.set_link(pipe_.read);
  }

  static constexpr auto kPipeData = "hello world"sv;

 protected:
  unique_pipe pipe_;
};

TEST_F(LinkedPipeReadProxyTest, NothingJustLink) {}

TEST_F(LinkedPipeReadProxyTest, ReadFromReadyPipe) {
  write(pipe_.write, kPipeData);
  EXPECT_EQ(read(proxy_), kPipeData);
}

TEST_F(LinkedPipeReadProxyTest, ReadFromSharedPipeFirst) {
  write(pipe_.write, kPipeData);
  EXPECT_EQ(read(proxy_, 5), "hello"sv);
  EXPECT_EQ(read(pipe_.read), " world"sv);
}

TEST_F(LinkedPipeReadProxyTest, ReadFromSharedPipeSecond) {
  write(pipe_.write, kPipeData);
  EXPECT_EQ(read(pipe_.read, 5), "hello"sv);
  EXPECT_EQ(read(proxy_), " world"sv);
}

TEST_F(LinkedPipeReadProxyTest, ReadFromPipeMultiple) {
  write(pipe_.write, kPipeData);
  EXPECT_EQ(read(proxy_, 5), "hello"sv);
  EXPECT_EQ(read(proxy_, 1), " "sv);
  EXPECT_EQ(read(proxy_, 4), "worl"sv);
  EXPECT_EQ(read(proxy_), "d"sv);
}

TEST_F(LinkedPipeReadProxyTest, ReadFromEmptyPipeBlocks) {
  static bool signaled{}; // atomic not needed - using a signal, not a thread
  signal_handler alarm_handler{SIGALRM, +[](int) { signaled = true; }};
  alarm(1);
  EXPECT_ERRNO(read(proxy_), EINTR);
  EXPECT_TRUE(signaled);
}

TEST_F(LinkedPipeReadProxyTest, ClearInflightRead) {
  std::jthread waiter{[&] {
    std::this_thread::sleep_for(1s);
    control_.clear_link();
  }};
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
}

TEST_F(LinkedPipeReadProxyTest, ChangeThenClearInflightRead) {
  std::jthread waiter{[&] {
    std::this_thread::sleep_for(1s);
    control_.set_link_raw(STDIN_FILENO);
    std::this_thread::sleep_for(1s);
    control_.clear_link();
  }};
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
  EXPECT_ERRNO(read(proxy_), ENOTCONN);
}

struct LinkedStdinIoctlProxyTest : ControlledProxyTest {
  LinkedStdinIoctlProxyTest() {
    control_.set_link_raw(STDIN_FILENO);
  }
};

TEST_F(LinkedStdinIoctlProxyTest, TtyIoctl) {
  struct termios attrs;
  errno_check(ioctl, proxy_, TCGETS, &attrs);
}

struct MultiProcessProxyTest : ControlledProxyTest {
  static constexpr auto kPrivatePath{"/root/private_data"sv};
};

TEST_F(MultiProcessProxyTest, DroppedPermissionChildCannotReadPrivateData) {
  forktest([](auto const&, auto const&){
    // no parent setup needed
  },
  [](auto const&, auto const&){
    EXPECT_ERRNO(errno_check(open, kPrivatePath.data(), O_RDONLY), EACCES);
  });
}

TEST_F(MultiProcessProxyTest, MultiProcess) {
  forktest([&](auto const& incoming, auto const& outgoing) {
    proxy_.close();

    unique_fd root_private_file{errno_check(open, kPrivatePath.data(), O_RDONLY)};
    auto private_data = read(root_private_file);
    errno_check(lseek, root_private_file.fd(), SEEK_SET, 0);
    
    control_.set_link(root_private_file);
    write(outgoing, kReady);

    EXPECT_EQ(read(incoming), private_data);
  },
  [&](auto const& incoming, auto const& outgoing) {
    control_.fd().close();

    EXPECT_EQ(read(incoming), kReady);
    write(outgoing, read(proxy_));
  });
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  (void)RUN_ALL_TESTS();

  reboot(RB_POWER_OFF);
}
