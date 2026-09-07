#include "logger.h"

#include <ctime>
#include <filesystem>
#include <vector>

#include <algorithm>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "ui_log.h"

namespace fs = std::filesystem;

/* 自定义 sink：按级别把日志推到 UI 双缓冲(info / debug+error) */
class UiLogSink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg &msg) override {
    std::string s(msg.payload.data(), msg.payload.size());
    if (msg.level == spdlog::level::info) {
      ui_log::push_info(s);
    } else if (msg.level == spdlog::level::debug ||
               msg.level == spdlog::level::err ||
               msg.level == spdlog::level::warn) {
      const char *tag =
          (msg.level == spdlog::level::err)     ? "[ERR] "
          : (msg.level == spdlog::level::warn)  ? "[WARN] "
                                                : "[DBG] ";
      ui_log::push_dbgerr(tag + s);
    }
  }
  void flush_() override {}
};

namespace logger {

/* 清理旧日志:按名字排序(时间戳前缀),仅保留最新 max_keep 个 run */
static void prune_old_logs(const std::string &log_dir, size_t max_keep) {
  std::vector<fs::path> runs;
  std::error_code ec;
  for (auto &e : fs::directory_iterator(log_dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    auto name = e.path().filename().string();
    /* 主文件形如 20260905_144002.txt; 滚动分片带 .1.txt 后缀, 同属一个 run */
    if (name.rfind("20", 0) == 0 && name.find(".txt") != std::string::npos)
      runs.push_back(e.path());
  }
  if (runs.size() <= max_keep) return;
  std::sort(runs.begin(), runs.end());
  size_t remove_n = runs.size() - max_keep;
  for (size_t i = 0; i < remove_n; i++) {
    fs::remove(runs[i], ec);
    fprintf(stderr, "[logger] prune old log: %s\n", runs[i].c_str());
  }
}

void init(const std::string &log_dir) {
  fs::create_directories(log_dir);
  prune_old_logs(log_dir, 10);
  std::time_t now = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
  std::string path = log_dir + "/" + ts + ".txt";

  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::info);
  /* 滚动文件: 单个 50MB, 最多 5 个分片, 单次运行最多约 250MB */
  auto file_sink =
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, 50 * 1024 * 1024, 5);
  file_sink->set_level(spdlog::level::trace);
  auto ui_sink = std::make_shared<UiLogSink>();
  ui_sink->set_level(spdlog::level::debug);

  auto lg = std::make_shared<spdlog::logger>(
      "app", spdlog::sinks_init_list{console_sink, file_sink, ui_sink});
  lg->set_level(spdlog::level::debug);
  lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  lg->flush_on(spdlog::level::debug);
  spdlog::set_default_logger(lg);
  spdlog::info("spdlog initialized, log file: {}", path);
}

}  // namespace logger
