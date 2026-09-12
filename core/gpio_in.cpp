/* GPIO 输入模块 —— VR58H3 板载 DI(命名节点 /sys/class/gpio/gpiof_inN)。
 * 对端只发一次信号(单脉冲), 因此:
 *  - read_fast() 用持久 fd + pread, 单次读取约几微秒, 支撑 500us 级轮询
 *  - 脉冲锁存由调用方轮询线程实现(连续采样到 HIGH 即锁存暂停) */
#include "gpio_in.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>

namespace gpio_in {

static int g_value_fd = -1;
static int g_ch = -1;
static std::atomic<bool> g_paused{false};

bool init(int ch) {
  /* ch 为板子丝印脚号 1-4 (DI1-DI4), 节点序号为 ch-1 */
  if (ch < 1 || ch > 4) {
    SPDLOG_WARN("GPIO-in: 脚号 {} 非法(有效 1-4)", ch);
    return false;
  }
  std::string path =
      "/sys/class/gpio/gpiof_in" + std::to_string(ch - 1) + "/value";
  g_value_fd = open(path.c_str(), O_RDONLY);
  if (g_value_fd < 0) {
    SPDLOG_WARN("GPIO-in: 打开 {} 失败: {}", path, strerror(errno));
    return false;
  }
  g_ch = ch;
  SPDLOG_INFO("GPIO-in: DI{} ({}) 就绪, 高频轮询模式", ch, path);
  std::atexit(+[] { shutdown(); });
  return true;
}

int read_fast() {
  if (g_value_fd < 0) return -1;
  char c = '0';
  if (pread(g_value_fd, &c, 1, 0) != 1) return -1;
  return (c == '1') ? 1 : 0;
}

int read() {
  if (g_ch < 0) return -1;
  std::string path =
      "/sys/class/gpio/gpiof_in" + std::to_string(g_ch - 1) + "/value";
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return -1;
  char buf[4];
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  return (buf[0] == '1') ? 1 : 0;
}

void shutdown() {
  if (g_value_fd >= 0) {
    close(g_value_fd);
    g_value_fd = -1;
  }
  g_ch = -1;
}

bool is_system_paused() { return g_paused.load(); }
void set_paused(bool v) { g_paused.store(v); }

}  // namespace gpio_in
