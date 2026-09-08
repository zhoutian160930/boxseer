#include "can_bus.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "affinity.h"

namespace can_bus {

static int g_send_fd = -1;
static int g_recv_fd = -1;
static int g_can_id = 0x300;
static std::atomic<bool> g_running{false};
static std::thread g_recv_thread;

static int open_can(const std::string &ifname) {
  int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd < 0) {
    SPDLOG_WARN("CAN: socket() 失败: {}", std::strerror(errno));
    return -1;
  }
  ifreq ifr;
  std::memset(&ifr, 0, sizeof(ifr));
  std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    SPDLOG_WARN("CAN: 接口 {} 不存在: {}", ifname, std::strerror(errno));
    close(fd);
    return -1;
  }
  sockaddr_can addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
    SPDLOG_WARN("CAN: bind {} 失败: {}", ifname, std::strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

/* 接收线程：非阻塞轮询读 can1，打印 logger 用于本地回环验证 */
static void recv_loop() {
  can_frame frame;
  while (g_running.load()) {
    ssize_t n = read(g_recv_fd, &frame, sizeof(frame));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        usleep(50000);  // 50ms 轮询
        continue;
      }
      SPDLOG_WARN("CAN: 接收读错误: {}", std::strerror(errno));
      break;
    }
    if (n == sizeof(frame)) {
      SPDLOG_INFO("[CAN-recv] id=0x{:03X} dlc={} data[0]=0x{:02X}",
                  frame.can_id, frame.can_dlc,
                  frame.can_dlc > 0 ? frame.data[0] : 0xFF);
    }
  }
}

bool init(const std::string &send_if, const std::string &recv_if, int can_id) {
  g_can_id = can_id;
  g_send_fd = open_can(send_if);
  if (g_send_fd < 0) {
    SPDLOG_WARN("CAN: 发送接口 {} 不可用，结果发送将禁用", send_if);
  } else {
    SPDLOG_INFO("CAN: 发送接口 {} 就绪, can_id=0x{:03X}", send_if, can_id);
  }
  g_recv_fd = open_can(recv_if);
  if (g_recv_fd >= 0) {
    int flags = fcntl(g_recv_fd, F_GETFL, 0);
    fcntl(g_recv_fd, F_SETFL, flags | O_NONBLOCK);  // 非阻塞
    SPDLOG_INFO("CAN: 接收接口 {} 就绪", recv_if);
    g_running = true;
    g_recv_thread = std::thread([] {
      affinity::pin_small();  /* CAN 收发钉到 A55 小核,不占推理大核 */
      recv_loop();
    });
    std::atexit(+[] { shutdown(); });  /* 进程退出时自动回收线程(Ui/headless通用) */
  } else {
    SPDLOG_WARN("CAN: 接收接口 {} 不可用，仅发送模式", recv_if);
  }
  return g_send_fd >= 0;
}

void send_result(bool ok) {
  if (g_send_fd < 0) return;
  can_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.can_id = g_can_id;
  frame.can_dlc = 1;
  frame.data[0] = ok ? 0x00 : 0x01;  // 满足=0, 不满足=1
  ssize_t n = write(g_send_fd, &frame, sizeof(frame));
  if (n < 0) {
    SPDLOG_ERROR("[CAN-send] 发送失败(id=0x{:03X}): {}", g_can_id,
                 std::strerror(errno));
  } else {
    SPDLOG_INFO("[CAN-send] id=0x{:03X} data=0x{:02X} ({})", g_can_id,
                frame.data[0], ok ? "满足" : "不满足");
  }
}

void shutdown() {
  g_running = false;
  if (g_recv_thread.joinable()) g_recv_thread.join();
  if (g_recv_fd >= 0) { close(g_recv_fd); g_recv_fd = -1; }
  if (g_send_fd >= 0) { close(g_send_fd); g_send_fd = -1; }
}

}  // namespace can_bus
