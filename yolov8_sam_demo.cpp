#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <opencv2/opencv.hpp>
#include <unistd.h>
#include "affinity.h"
#include "rknn_pool.h"
#include "mobilesam/mobilesam_pool.h"
#include "image_process.h"
#include <filesystem>
#include <chrono>   
#include <algorithm>

#if WITH_UI
#include "ui_app.h"
#endif
#include "app_config.h"
#include "config.h"
#include "logger.h"
#include "can_bus.h"
#include "gpio_out.h"
#include "gpio_in.h"
#include "judgment.h"
#include <spdlog/spdlog.h>
#include <thread>
typedef std::chrono::high_resolution_clock Clock;
typedef std::chrono::milliseconds Milliseconds;
// ===== 新增：微秒类型，统计耗时更精准，可选保留 =====
typedef std::chrono::microseconds Microseconds;

const std::string YOLO_MODEL_PATH = "model/yolov8s.rknn"; 
const std::string SAM_ENCODER_PATH = "model/mobile_sam_encoder.rknn";
const std::string SAM_DECODER_PATH = "model/mobile_sam_decoder.rknn";
const std::string LABEL_PATH = "model/coco_80_labels_list.txt";

int main(int argc, char **argv) {
#if WITH_UI
    config::init(config::exe_dir() + "/../config", "parameters.json");
    logger::init(config::g.log_root);             /* 日志目录由配置决定 */
    SPDLOG_INFO("program start, argc={}", argc);
    SPDLOG_INFO("config: material={} box={} target={} lines={:.2f}/{:.2f}",
                config::g.material_class, config::g.box_class,
                config::g.target_count, config::g.line_left_frac,
                config::g.line_right_frac);
    if (config::g.can_enabled)
        can_bus::init(config::g.can_send_if, config::g.can_recv_if,
                      config::g.can_id);
    if (config::g.gpio_enabled)
        gpio_out::init(config::g.gpio_out_ch);
    if (config::g.gpio_input_enabled) {
        gpio_in::init(config::g.gpio_input_ch);
        std::thread([] {
            affinity::pin_cpu(0);  /* GPIO 轮询独占小核0 */
            /* 高频轮询捕捉对端单脉冲: 连续2次HIGH(去抖)即认定有效 */
            int high_run = 0;
            bool latched = false;
            bool was_paused = false;
            int poll_us = std::max(100, config::g.gpio_poll_us);
            SPDLOG_INFO("[GPIO-ctrl] DI{} 轮询线程启动 ({}us)",
                        config::g.gpio_input_ch, poll_us);
            while (true) {
                int v = gpio_in::read_fast();
                high_run = (v == 1) ? high_run + 1 : 0;
                if (!latched && high_run >= 2) {
                    latched = true;
                    SPDLOG_WARN("[GPIO-ctrl] DI{} 捕捉到脉冲 → 系统暂停",
                                config::g.gpio_input_ch);
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
    /* 无参数直接启动 UI（输入源在界面内用文件浏览器选择） */
    if (argc < 2) {
        AppConfig cfg;
        cfg.input_path = "";
        return run_ui_mode(cfg);
    }
#else
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <image_or_video_or_dir> [yolo_model] [sam_encoder] [sam_decoder] [label_path] [--out-dir OUT_DIR] [--yolo-threads N] [--sam-threads M] [--no-sam] [--headless]" << std::endl;
        return -1;
    }
#endif

    std::string input_path = argv[1];
    std::string yolo_path = YOLO_MODEL_PATH;
    std::string sam_enc_path = SAM_ENCODER_PATH;
    std::string sam_dec_path = SAM_DECODER_PATH;
    std::string label_path = LABEL_PATH;
    std::string out_dir = "outputs";
    int yolo_threads = 3;
    int sam_threads = 3;
    bool use_sam = true;
    int ui_flag = -1; /* -1: 自动, 0: 强制无头, 1: 强制 UI */

    int positional_arg_index = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--yolo-threads" && i + 1 < argc) {
            yolo_threads = std::atoi(argv[++i]);
        } else if (arg == "--sam-threads" && i + 1 < argc) {
            sam_threads = std::atoi(argv[++i]);
        } else if (arg == "--no-sam") {
            use_sam = false;
        } else if (arg == "--headless") {
            ui_flag = 0;
        } else if (arg == "--ui") {
            ui_flag = 1;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Warning: Unknown option " << arg << std::endl;
        } else {
            switch (positional_arg_index) {
                case 0: yolo_path = arg; break;
                case 1: sam_enc_path = arg; break;
                case 2: sam_dec_path = arg; break;
                case 3: label_path = arg; break;
                default: break;
            }
            positional_arg_index++;
        }
    }
    std::filesystem::create_directories(out_dir);

    std::string result_suffix = use_sam ? "_sam.jpg" : "_det.jpg";
    std::string video_prefix = use_sam ? "sam_out_" : "det_out_";

#if WITH_UI
    // UI 模式：默认根据 DISPLAY 自动判断(ssh -X 有 DISPLAY 则 UI，否则无头)；
    // 可用 --ui / --headless 强制覆盖。loop.sh 等无 DISPLAY 场景仍走无头。
    bool want_ui = (ui_flag >= 0) ? (ui_flag == 1) : (getenv("DISPLAY") != nullptr);
    if (want_ui) {
        AppConfig cfg;
        cfg.input_path = input_path;
        cfg.yolo_path = yolo_path;
        cfg.sam_enc_path = sam_enc_path;
        cfg.sam_dec_path = sam_dec_path;
        cfg.label_path = label_path;
        cfg.out_dir = out_dir;
        cfg.yolo_threads = yolo_threads;
        cfg.sam_threads = sam_threads;
        cfg.use_sam = use_sam;
        return run_ui_mode(cfg);
    }
#endif

    // ===================== 【1】所有模型加载/初始化都在这里，这部分耗时完全剔除 =====================
    printf("Initializing YOLOv8 Pool with model: %s, threads: %d\n", yolo_path.c_str(), yolo_threads);
    printf("Using label file: %s\n", label_path.c_str());
    RknnPool yolo_pool(yolo_path, yolo_threads, label_path);
    
    std::unique_ptr<MobileSamPool> sam_pool;
    if (use_sam) {
        printf("Initializing MobileSAM Pool with encoder: %s, decoder: %s, threads: %d\n", sam_enc_path.c_str(), sam_dec_path.c_str(), sam_threads);
        sam_pool = std::make_unique<MobileSamPool>(sam_enc_path, sam_dec_path, sam_threads);
        if (sam_pool->Init() != 0) {
            std::cerr << "Failed to init MobileSAM pool" << std::endl;
            return -1;
        }
    } else {
        printf("SAM post-processing is DISABLED (--no-sam). Running YOLO detection only.\n");
    }

    cv::Mat img = cv::imread(input_path);
    bool is_image = !img.empty();
    bool is_dir = std::filesystem::is_directory(input_path);
    cv::VideoCapture cap;
    if (!is_image && !is_dir) {
        cap.open(input_path);
        if (!cap.isOpened()) {
            std::cerr << "Error opening media: " << input_path << std::endl;
            return -1;
        }
    }

    int frame_count = 0;
    int sam_submission_count = 0;
    int yolo_received_count = 0;

    std::cout << "Starting inference pipeline..." << std::endl;
    // ===================== 【2】关键修改：把计时开始放在【所有初始化完成后、推理任务开始前】 =====================
    auto total_start_time = Clock::now();  // 只统计推理+前后处理，模型加载耗时已完全剔除

    if (is_image) {
        auto src = std::make_shared<cv::Mat>(img.clone());
        std::string base = std::filesystem::path(input_path).stem().string();
        std::string out_name = (std::filesystem::path(out_dir) / (base + result_suffix)).string();
        ImageProcess image_process(src->cols, src->rows, 640);
        yolo_pool.AddInferenceTask(src, image_process, out_name);
        frame_count++;
    } else if (is_dir) {
        std::vector<std::filesystem::path> files;
        for (auto& p : std::filesystem::directory_iterator(input_path)) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().string();
            for (auto& c : ext) c = std::tolower(c);
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") {
                files.push_back(p.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (auto& p : files) {
            cv::Mat im = cv::imread(p.string());
            if (im.empty()) continue;
            auto src = std::make_shared<cv::Mat>(im);
            std::string base = p.stem().string();
            std::string out_name = (std::filesystem::path(out_dir) / (base + result_suffix)).string();
            ImageProcess image_process(src->cols, src->rows, 640);
            yolo_pool.AddInferenceTask(src, image_process, out_name);
            frame_count++;
            if (yolo_pool.GetTasksSize() > yolo_threads * 2) {
                usleep(10000);
            }
        }
    }

    while (true) {
        /* 外部暂停控制：P24=HIGH 时阻塞等待恢复 */
        while (gpio_in::is_system_paused()) {
            usleep(200000);
        }
        cv::Mat frame;
        if (!is_image && !is_dir) {
            cap >> frame;
            if (!frame.empty()) {
                auto src = std::make_shared<cv::Mat>(frame.clone());
                static int seq = 0;
                std::string out_name = (std::filesystem::path(out_dir) / (video_prefix + std::to_string(seq++) + ".jpg")).string();
                ImageProcess image_process(src->cols, src->rows, 640);
                yolo_pool.AddInferenceTask(src, image_process, out_name);
                frame_count++;
            }
        }
        
        YoloResult yolo_res = yolo_pool.GetImageResultFromQueue();
        if (yolo_res.img) {
            std::string out_name = yolo_res.tag.empty()
                ? (std::filesystem::path(out_dir) / (use_sam ? "sam_out.jpg" : "det_out.jpg")).string()
                : yolo_res.tag;
            yolo_received_count++;

            /* 多盒判定 + CAN 发送(与 UI worker 共用同一判定逻辑) */
            if (config::g.can_enabled && yolo_res.img) {
              auto boxes =
                  judge_all_boxes(yolo_res.results, yolo_res.img->cols,
                                  (float)config::g.line_left_frac,
                                  (float)config::g.line_right_frac,
                                  config::g.target_count,
                                  config::g.box_class,
                                  config::g.material_class);
              JudgeSummary sum = summarize(boxes);
              bool ok = is_qualified(sum);
              can_bus::send_result(ok);
              gpio_out::set_qualified(ok);
              SPDLOG_INFO("frame {} qualified={} full={} notfull={} notrevealed={}",
                          yolo_received_count, ok, sum.full, sum.not_full,
                          sum.not_revealed);
            }

            if (use_sam) {
                std::vector<mobilesam_box> sam_boxes;
                for (int i = 0; i < yolo_res.results.count; i++) {
                    object_detect_result& det = yolo_res.results.results[i];
                    mobilesam_box box;
                    box.x1 = det.box.left;
                    box.y1 = det.box.top;
                    box.x2 = det.box.right;
                    box.y2 = det.box.bottom;
                    sam_boxes.push_back(box);
                }
                printf("Saving SAM result to: %s\n", out_name.c_str());
                sam_pool->AddInferenceTask(*yolo_res.img, out_name, sam_boxes);
                sam_submission_count++;
            } else {
                cv::Mat out_img = yolo_res.img->clone();
                ImageProcess draw_process(out_img.cols, out_img.rows, 640);
                draw_process.ImagePostProcess(out_img, yolo_res.results);
                printf("Saving detection result to: %s\n", out_name.c_str());
                cv::imwrite(out_name, out_img);
            }
        }
        
        bool sam_done = !use_sam || (sam_pool->GetProcessedCount() >= sam_submission_count);

        if (is_image || is_dir) {
            if (yolo_received_count >= frame_count && sam_done) {
                break;
            }
            usleep(10000);
        } else {
            if (frame.empty() && yolo_pool.GetTasksSize() == 0) {
                if (sam_done) {
                    break;
                }
                usleep(10000);
            }
        }
        
        if (yolo_pool.GetTasksSize() > yolo_threads * 2) {
            usleep(10000);
        }
    }

    // ===================== 【3】关键修改：计时结束放在【所有推理任务+后处理+保存完成后】 =====================
    auto total_end_time = Clock::now();
    Milliseconds total_ms = std::chrono::duration_cast<Milliseconds>(total_end_time - total_start_time);
    float avg_frame_ms = frame_count > 0 ? (float)total_ms.count() / frame_count : 0.0f;

    // ===================== 【4】可选优化：增加 总秒数 和 FPS 统计，更直观 =====================
    float total_seconds = total_ms.count() / 1000.0f;
    float fps = frame_count > 0 ? frame_count / total_seconds : 0.0f;

    long long yolo_pre_us = yolo_pool.GetYoloPreprocessTimeUs();
    long long yolo_infer_us = yolo_pool.GetYoloInferenceTimeUs();
    long long sam_pre_us = use_sam ? sam_pool->GetSamPreprocessTimeUs() : 0;
    long long sam_infer_us = use_sam ? sam_pool->GetSamInferenceTimeUs() : 0;
    long long sam_post_us = use_sam ? sam_pool->GetSamPostprocessTimeUs() : 0;
    double yolo_pre_ms = yolo_pre_us / 1000.0;
    double yolo_infer_ms = yolo_infer_us / 1000.0;
    double sam_pre_ms = sam_pre_us / 1000.0;
    double sam_infer_ms = sam_infer_us / 1000.0;
    double sam_post_ms = sam_post_us / 1000.0;
    double total_pre_ms = (yolo_pre_us + sam_pre_us) / 1000.0;
    double total_post_ms = sam_post_us / 1000.0;
    double total_prepost_ms = (yolo_pre_us + sam_pre_us + sam_post_us) / 1000.0;
    double total_infer_ms = (yolo_infer_us + sam_infer_us) / 1000.0;
    double avg_yolo_pre_ms = frame_count > 0 ? yolo_pre_ms / frame_count : 0.0;
    double avg_yolo_infer_ms = frame_count > 0 ? yolo_infer_ms / frame_count : 0.0;
    double avg_sam_pre_ms = frame_count > 0 ? sam_pre_ms / frame_count : 0.0;
    double avg_sam_infer_ms = frame_count > 0 ? sam_infer_ms / frame_count : 0.0;
    double avg_sam_post_ms = frame_count > 0 ? sam_post_ms / frame_count : 0.0;

    std::cout << "=============================================" << std::endl;
    std::cout << "Pipeline finished successfully!" << std::endl;
    std::cout << "Total processed frames: " << frame_count << std::endl;
    std::cout << "Total process time (预处理+推理+后处理): " << total_ms.count() << " ms (" << total_seconds << " s)" << std::endl;
    std::cout << "Average time per frame: " << avg_frame_ms << " ms" << std::endl;
    std::cout << "Average FPS: " << fps << std::endl;
    std::cout << "---------------------------------------------" << std::endl;
    std::cout << "YOLO 预处理累计: " << yolo_pre_ms << " ms, 平均: " << avg_yolo_pre_ms << " ms/帧" << std::endl;
    std::cout << "YOLO 推理累计: " << yolo_infer_ms << " ms, 平均: " << avg_yolo_infer_ms << " ms/帧" << std::endl;
    std::cout << "MobileSAM 预处理累计: " << sam_pre_ms << " ms, 平均: " << avg_sam_pre_ms << " ms/帧" << std::endl;
    std::cout << "MobileSAM 推理累计: " << sam_infer_ms << " ms, 平均: " << avg_sam_infer_ms << " ms/帧" << std::endl;
    std::cout << "MobileSAM 后处理累计: " << sam_post_ms << " ms, 平均: " << avg_sam_post_ms << " ms/帧" << std::endl;
    std::cout << "图像前处理累计: " << total_pre_ms << " ms" << std::endl;
    std::cout << "图像后处理累计: " << total_post_ms << " ms" << std::endl;
    std::cout << "图像前后处理累计: " << total_prepost_ms << " ms" << std::endl;
    std::cout << "模型推理累计: " << total_infer_ms << " ms" << std::endl;
    std::cout << "=============================================" << std::endl;

    return 0;
}
