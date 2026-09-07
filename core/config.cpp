#include "config.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <limits.h>

namespace fs = std::filesystem;

namespace config {

Config g;
std::string file_path;

std::string exe_dir() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  std::string p(buf);
  size_t pos = p.find_last_of('/');
  return pos == std::string::npos ? "." : p.substr(0, pos);
}

static long last_mtime = 0;          /* 上次已知文件 mtime */
static bool dirty = false;
static auto last_save_time = std::chrono::steady_clock::now();

/* ---------- 极简扁平 JSON 解析（每行 "key": value） ---------- */

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n,");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n,");
  return s.substr(a, b - a + 1);
}

/* 从一行里提取 "key" 与其后的 value。成功返回 true。 */
static bool parse_line(std::string line, std::string &key, std::string &val) {
  line = trim(line);
  if (line.empty() || line == "{" || line == "}") return false;
  /* key: 首个 "..." */
  size_t q1 = line.find('"');
  if (q1 == std::string::npos) return false;
  size_t q2 = line.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  key = line.substr(q1 + 1, q2 - q1 - 1);
  size_t colon = line.find(':', q2 + 1);
  if (colon == std::string::npos) return false;
  val = trim(line.substr(colon + 1));
  /* 字符串值去掉两端引号 */
  if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
    val = val.substr(1, val.size() - 2);
  return true;
}

static void apply_kv(const std::string &k, const std::string &v) {
  if (k == "material_class") g.material_class = std::atoi(v.c_str());
  else if (k == "box_class") g.box_class = std::atoi(v.c_str());
  else if (k == "target_count") g.target_count = std::atoi(v.c_str());
  else if (k == "line_left_frac") g.line_left_frac = std::atof(v.c_str());
  else if (k == "line_right_frac") g.line_right_frac = std::atof(v.c_str());
  else if (k == "yolo_model") g.yolo_model = v;
  else if (k == "label_path") g.label_path = v;
  else if (k == "sam_encoder") g.sam_encoder = v;
  else if (k == "sam_decoder") g.sam_decoder = v;
  else if (k == "detImg_root") g.detImg_root = v;
  else if (k == "saveImg_root") g.saveImg_root = v;
  else if (k == "log_root") g.log_root = v;
  else if (k == "yolo_threads") g.yolo_threads = std::atoi(v.c_str());
  else if (k == "use_sam") g.use_sam = (v == "true" || v == "1");
  else if (k == "sam_threads") g.sam_threads = std::atoi(v.c_str());
  else if (k == "default_input") g.default_input = v;
  else if (k == "fb_model_dir") g.fb_model_dir = v;
  else if (k == "fb_input_dir") g.fb_input_dir = v;
  else if (k == "font_path") g.font_path = v;
  else if (k == "ui_width") g.ui_width = std::atoi(v.c_str());
  else if (k == "ui_height") g.ui_height = std::atoi(v.c_str());
  else if (k == "panel_width") g.panel_width = std::atoi(v.c_str());
  else if (k == "default_res") g.default_res = std::atoi(v.c_str());
  else if (k == "capture_interval_ms") g.capture_interval_ms = std::atoi(v.c_str());
  else if (k == "can_enabled") g.can_enabled = (v == "true" || v == "1");
  else if (k == "can_send_if") g.can_send_if = v;
  else if (k == "can_recv_if") g.can_recv_if = v;
  else if (k == "can_id") g.can_id = (int)strtol(v.c_str(), nullptr, 0);
  else if (k == "gpio_enabled") g.gpio_enabled = (v == "true" || v == "1");
  else if (k == "gpio_out_ch") g.gpio_out_ch = std::atoi(v.c_str());
  else if (k == "gpio_input_enabled") g.gpio_input_enabled = (v == "true" || v == "1");
  else if (k == "gpio_input_ch") g.gpio_input_ch = std::atoi(v.c_str());
  else if (k == "gpio_poll_us") g.gpio_poll_us = std::atoi(v.c_str());
  else if (k == "gpio_input_latch") g.gpio_input_latch = (v == "true" || v == "1");
  else if (k == "camera_enabled") g.camera_enabled = (v == "true" || v == "1");
  else if (k == "camera_iface") g.camera_iface = v;
   else if (k == "camera_ip") g.camera_ip = v;
   else if (k == "camera_timeout_ms") g.camera_timeout_ms = std::atoi(v.c_str());
  else if (k == "camera_width") g.camera_width = std::atoi(v.c_str());
  else if (k == "camera_height") g.camera_height = std::atoi(v.c_str());
  else if (k == "camera_exposure_us") g.camera_exposure_us = std::atoi(v.c_str());
  else if (k == "camera_gain") g.camera_gain = std::atof(v.c_str());
}

static bool load_file() {
  std::ifstream in(file_path);
  if (!in.is_open()) return false;
  std::string line;
  while (std::getline(in, line)) {
    std::string k, v;
    if (parse_line(line, k, v)) apply_kv(k, v);
  }
  return true;
}

/* ---------- 回写：每行一个参数 ---------- */

static void write_file() {
  fs::create_directories(fs::path(file_path).parent_path());
  std::ofstream out(file_path, std::ios::trunc);
  if (!out.is_open()) return;
  out << "{\n";
  out << "  \"material_class\": " << g.material_class << ",\n";
  out << "  \"box_class\": " << g.box_class << ",\n";
  out << "  \"target_count\": " << g.target_count << ",\n";
  out << "  \"line_left_frac\": " << g.line_left_frac << ",\n";
  out << "  \"line_right_frac\": " << g.line_right_frac << ",\n";
  out << "  \"yolo_model\": \"" << g.yolo_model << "\",\n";
  out << "  \"label_path\": \"" << g.label_path << "\",\n";
  out << "  \"sam_encoder\": \"" << g.sam_encoder << "\",\n";
  out << "  \"sam_decoder\": \"" << g.sam_decoder << "\",\n";
  out << "  \"detImg_root\": \"" << g.detImg_root << "\",\n";
  out << "  \"saveImg_root\": \"" << g.saveImg_root << "\",\n";
  out << "  \"log_root\": \"" << g.log_root << "\",\n";
  out << "  \"yolo_threads\": " << g.yolo_threads << ",\n";
  out << "  \"use_sam\": " << (g.use_sam ? "true" : "false") << ",\n";
  out << "  \"sam_threads\": " << g.sam_threads << ",\n";
  out << "  \"default_input\": \"" << g.default_input << "\",\n";
  out << "  \"fb_model_dir\": \"" << g.fb_model_dir << "\",\n";
  out << "  \"fb_input_dir\": \"" << g.fb_input_dir << "\",\n";
  out << "  \"font_path\": \"" << g.font_path << "\",\n";
  out << "  \"ui_width\": " << g.ui_width << ",\n";
  out << "  \"ui_height\": " << g.ui_height << ",\n";
  out << "  \"panel_width\": " << g.panel_width << ",\n";
  out << "  \"default_res\": " << g.default_res << ",\n";
  out << "  \"capture_interval_ms\": " << g.capture_interval_ms << ",\n";
  out << "  \"can_enabled\": " << (g.can_enabled ? "true" : "false") << ",\n";
  out << "  \"can_send_if\": \"" << g.can_send_if << "\",\n";
  out << "  \"can_recv_if\": \"" << g.can_recv_if << "\",\n";
  out << "  \"can_id\": " << g.can_id << ",\n";
  out << "  \"gpio_enabled\": " << (g.gpio_enabled ? "true" : "false") << ",\n";
  out << "  \"gpio_out_ch\": " << g.gpio_out_ch << ",\n";
  out << "  \"gpio_input_enabled\": " << (g.gpio_input_enabled ? "true" : "false") << ",\n";
  out << "  \"gpio_input_ch\": " << g.gpio_input_ch << ",\n";
  out << "  \"gpio_poll_us\": " << g.gpio_poll_us << ",\n";
  out << "  \"gpio_input_latch\": " << (g.gpio_input_latch ? "true" : "false") << ",\n";
  out << "  \"camera_enabled\": " << (g.camera_enabled ? "true" : "false") << ",\n";
  out << "  \"camera_iface\": \"" << g.camera_iface << "\",\n";
   out << "  \"camera_ip\": \"" << g.camera_ip << "\",\n";
   out << "  \"camera_timeout_ms\": " << g.camera_timeout_ms << ",\n";
   out << "  \"camera_width\": " << g.camera_width << ",\n";
   out << "  \"camera_height\": " << g.camera_height << ",\n";
   out << "  \"camera_exposure_us\": " << g.camera_exposure_us << ",\n";
   out << "  \"camera_gain\": " << g.camera_gain << "\n";
  out << "}\n";
}

/* ---------- 公共 API ---------- */

void init(const std::string &config_dir, const std::string &file) {
  fs::create_directories(config_dir);
  file_path = config_dir + "/" + file;
  if (!load_file()) {
    write_file();  /* 不存在则生成默认 */
    fprintf(stderr, "[config] generated default: %s\n", file_path.c_str());
  } else {
    fprintf(stderr, "[config] loaded: %s\n", file_path.c_str());
  }
  if (fs::exists(file_path))
    last_mtime = (long)fs::last_write_time(file_path).time_since_epoch().count();
}

void save() {
  write_file();
  if (fs::exists(file_path))
    last_mtime = (long)fs::last_write_time(file_path).time_since_epoch().count();
  dirty = false;
  last_save_time = std::chrono::steady_clock::now();
}

bool poll_hot_reload() {
  if (file_path.empty()) return false;
  std::error_code ec;
  auto t = (long)fs::last_write_time(file_path, ec).time_since_epoch().count();
  if (ec) return false;
  if (t != last_mtime) {
    if (load_file()) {
      last_mtime = t;
      dirty = false;
      return true;
    }
  }
  return false;
}

void mark_dirty() { dirty = true; }

bool poll_save_due() {
  if (!dirty) return false;
  auto now = std::chrono::steady_clock::now();
  if (now - last_save_time > std::chrono::seconds(1)) return true;
  return false;
}

}  // namespace config
