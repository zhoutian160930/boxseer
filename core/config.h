#pragma once
#include <string>

/* 全局配置（JSON 持久化 + 热加载）。扁平结构，每行一个参数。 */
struct Config {
  /* A: 任务/判定参数 */
  int material_class = 0;     /* 物料类别 ID */
  int box_class = 1;          /* 盒子类别 ID */
  int target_count = 10;      /* 满料目标物料数 */
  double line_left_frac = 0.25;   /* 左竖线占图宽比例 */
  double line_right_frac = 0.75;  /* 右竖线占图宽比例 */
  /* B: 路径 */
  std::string yolo_model = "model/yolov8s.rknn";
  std::string label_path = "model/coco_80_labels_list.txt";
  std::string sam_encoder = "model/mobile_sam_encoder.rknn";
  std::string sam_decoder = "model/mobile_sam_decoder.rknn";
  std::string detImg_root = "./output/detImg";
  std::string saveImg_root = "./output/saveImg";
  std::string log_root = "./output/log";
  int yolo_threads = 3;
  bool use_sam = false;
  int sam_threads = 3;
  std::string default_input;
  std::string fb_model_dir = "/home/ubuntu/lvgl/yolomodel";
  std::string fb_input_dir = "/home/ubuntu/lvgl";
  std::string font_path;
  int ui_width = 1280;
  int ui_height = 720;
  int panel_width = 320;
  int default_res = 0;
  /* 采集图像存图间隔(毫秒)。0=每帧都存。支持热加载。 */
  int capture_interval_ms = 200;
  /* CAN 通信 */
  bool can_enabled = true;            /* 是否启用 CAN 结果发送 */
  std::string can_send_if = "can0";   /* 发送接口 */
  std::string can_recv_if = "can1";   /* 接收接口(本地回环测试) */
  int can_id = 0x300;                 /* 标准 11-bit CAN ID */
  /* GPIO(VR58H3 板载 DO/DI, 命名节点 /sys/class/gpio/gpiof_outN / gpiof_inN)
   * DO 写值反相: 写0=高电平, 写1=低电平; 业务: 合格→低电平 */
  bool gpio_enabled = true;
  int gpio_out_ch = 1;                 /* DO 丝印脚号 1-4 (DO1~DO4) */
  bool gpio_input_enabled = true;
  int gpio_input_ch = 1;               /* DI 丝印脚号 1-4 (DI1~DI4) */
  int gpio_poll_us = 500;              /* DI 轮询周期(微秒), 对端单脉冲需高频捕捉 */
  bool gpio_input_latch = true;        /* true: 采到 HIGH 即锁定暂停(单脉冲); false: 电平跟随 */
  /* 摄像头(海康威视工业相机 via MVS SDK) */
  bool camera_enabled = false;
  std::string camera_iface = "eth1";
  std::string camera_ip = "192.168.1.100/24";
  int camera_timeout_ms = 10000;
  /* 相机采集分辨率。0=传感器原生满分辨率(每次启动显式复位，覆盖相机残留的脏配置)。
   * 若需 ROI 降分辨率提帧率，可设为如 1600/1200(保持 4:3，居中裁剪)。改动后需重启程序。 */
  int camera_width = 0;
  int camera_height = 0;
  /* 曝光时长(微秒)。0=自动曝光(默认);>0=手动曝光(关自动,写入相机,自动钳位到
   * 相机支持范围)。支持热加载:改 parameters.json 后立即生效,无需重启。
   * 参考范围(MV-CU013-80GC): 31~988930 us;现场偏暗就调大,如 50000=50ms。 */
  int camera_exposure_us = 0;
  /* 增益(dB)。-1=自动增益(默认);>=0=手动增益(关自动)。同样支持热加载。
   * 优先调曝光,曝光到顶仍不够亮再加增益(增益大会引入噪点)。 */
  double camera_gain = -1.0;
};

namespace config {

extern Config g;                  /* 全局配置实例 */
extern std::string file_path;     /* config.json 路径 */

/* 可执行文件所在目录(绝对路径,与启动时的工作目录无关)。 */
std::string exe_dir();

/* 初始化：加载 config/<file>；不存在则生成默认。 */
void init(const std::string &config_dir = "config",
          const std::string &file = "config.json");

/* 把内存中的当前值写回 JSON（pretty，每行一个参数）。 */
void save();

/* 主循环周期调用：若文件被外部编辑(mtime 变)则重新加载，返回 true。 */
bool poll_hot_reload();

/* 主循环周期调用：若有脏数据且距上次保存>1s，返回 true（调用方应先 sync 再 save）。 */
bool poll_save_due();

/* 标记内存值已改、需要回写（UI 改动后调用，实际写入会防抖节流）。 */
void mark_dirty();

/* 配方(仅产品相关参数快照): 存于 <config_dir>/recipes/<name>.json
 * 包含: yolo_model/label_path/material_class/box_class/target_count/
 *       line_left_frac/line_right_frac */
bool save_recipe(const std::string &name);   /* name 不得含路径分隔符 */
bool load_recipe(const std::string &name);   /* 加载进 config::g */
std::string recipes_dir();                   /* 配方目录路径 */
}
