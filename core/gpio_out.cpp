/* GPIO 输出模块 —— VR58H3 板载 DO(命名节点 /sys/class/gpio/gpiof_outN)。
 * 厂商固件已建好节点, 无需 export。DO 写值与物理电平反相:
 *   写 "0" → 高电平   写 "1" → 低电平
 * 业务语义(与旧 TCA6424 版一致): 合格→低电平, 不满足→高电平。 */
#include "gpio_out.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>

namespace gpio_out {

static int g_value_fd = -1;
static int g_ch = -1;
static int g_last_level = 0;  /* 物理电平 */

bool init(int ch) {
  /* ch 为板子丝印脚号 1-4 (DO1-DO4), 节点序号为 ch-1 */
  if (ch < 1 || ch > 4) {
    SPDLOG_WARN("GPIO-out: 脚号 {} 非法(有效 1-4)", ch);
    return false;
  }
  std::string path =
      "/sys/class/gpio/gpiof_out" + std::to_string(ch - 1) + "/value";
  g_value_fd = open(path.c_str(), O_WRONLY);
  if (g_value_fd < 0) {
    SPDLOG_WARN("GPIO-out: 打开 {} 失败: {}", path, strerror(errno));
    return false;
  }
  g_ch = ch;
  set_qualified(false);  /* 初始: 不满足 → 高电平 */
  SPDLOG_INFO("GPIO-out: DO{} ({}) 就绪, 初始 HIGH(不满足)", ch, path);
  std::atexit(+[] { shutdown(); });
  return true;
}

void set_qualified(bool ok) {
  if (g_value_fd < 0) return;
  /* 反相: 物理低电平写"1", 物理高电平写"0" */
  const char v = ok ? '1' : '0';
  if (pwrite(g_value_fd, &v, 1, 0) != 1) {
    SPDLOG_ERROR("[GPIO-out] 写 DO{} 失败: {}", g_ch, strerror(errno));
    return;
  }
  g_last_level = ok ? 0 : 1;
  SPDLOG_INFO("[GPIO-out] DO{}={} ({})", g_ch, ok ? "LOW" : "HIGH",
              ok ? "满足" : "不满足");
}

void shutdown() {
  if (g_value_fd >= 0) {
    set_qualified(false);  /* 退出恢复高电平(不满足), 与旧版语义一致 */
    close(g_value_fd);
    g_value_fd = -1;
    SPDLOG_INFO("GPIO-out: DO{} 已恢复 HIGH", g_ch);
    g_ch = -1;
  }
}

int last_output_level() { return g_last_level; }

}  // namespace gpio_out
