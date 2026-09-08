/* Qt 版入口:初始化顺序与旧 main 一致(config→logger→CAN→GPIO→GPIO轮询) */
#include <QApplication>

#include <spdlog/spdlog.h>

#include <thread>
#include <unistd.h>

#include "affinity.h"
#include "can_bus.h"
#include "config.h"
#include "gpio_in.h"
#include "gpio_out.h"
#include "logger.h"
#include "mainwindow.h"

int main(int argc, char *argv[]) {
  config::init(config::exe_dir() + "/../config", "parameters.json");
  logger::init(config::g.log_root);
  SPDLOG_INFO("==== yolov8_sam_demo (Qt UI) ====");

  if (config::g.can_enabled)
    can_bus::init(config::g.can_send_if, config::g.can_recv_if,
                  config::g.can_id);
  if (config::g.gpio_enabled) gpio_out::init(config::g.gpio_out_ch);
  if (config::g.gpio_input_enabled) {
    gpio_in::init(config::g.gpio_input_ch);
    std::thread([] {
      affinity::pin_cpu(0);  /* GPIO 轮询独占小核0, 低延迟且不扰大核推理 */
      /* 高频轮询捕捉对端单脉冲: 连续2次HIGH(去抖)即认定有效 */
      int high_run = 0;
      bool latched = false;
      bool was_paused = false;
      int poll_us = std::max(100, config::g.gpio_poll_us);
      SPDLOG_INFO("[GPIO-ctrl] DI{} 轮询线程启动 ({}us)",
                  config::g.gpio_input_ch + 1, poll_us);
      while (true) {
        int v = gpio_in::read_fast();
        high_run = (v == 1) ? high_run + 1 : 0;
        if (!latched && high_run >= 2) {
          latched = true;
          SPDLOG_WARN("[GPIO-ctrl] DI{} 捕捉到脉冲 → 系统暂停",
                      config::g.gpio_input_ch + 1);
        }
        bool paused = config::g.gpio_input_latch
                          ? latched
                          : (high_run >= 2);
        if (paused != was_paused) {
          gpio_in::set_paused(paused);
          was_paused = paused;
        }
        usleep(poll_us);
      }
    }).detach();
  }

  QApplication app(argc, argv);
  MainWindow w;
  /* 最大化启动(保留系统标题栏:右上角 最小化/缩放/关闭 三按钮;
   * 缩放=最大化↔窗口切换,关闭=走 closeEvent 清理退出) */
  w.showMaximized();
  return app.exec();
}
