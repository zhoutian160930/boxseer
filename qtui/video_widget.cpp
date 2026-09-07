#include "video_widget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>

#include <algorithm>

#include "pipeline.h"

namespace {
constexpr int kHitRadius = 12; /* 竖线命中半径(px) */
}

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent) {
  setMinimumSize(320, 180);
  setMouseTracking(false);
  setAutoFillBackground(true);
  QPalette pal = palette();
  pal.setColor(QPalette::Window, Qt::black);
  setPalette(pal);
  syncLinesToPipeline();
}

QRect VideoWidget::imageDisplayRect(const QSize &img, const QRect &vp) const {
  if (img.isEmpty()) return QRect();
  double scale = std::min((double)vp.width() / img.width(),
                          (double)vp.height() / img.height());
  int w = (int)(img.width() * scale);
  int h = (int)(img.height() * scale);
  int x = vp.x() + (vp.width() - w) / 2;
  int y = vp.y() + (vp.height() - h) / 2;
  return QRect(x, y, w, h);
}

void VideoWidget::setFrame(const cv::Mat &bgr) {
  if (bgr.empty()) return;
  cv::Mat rgb;
  cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
  if (disp_.cols != rgb.cols || disp_.rows != rgb.rows) {
    /* 尺寸变化:重新绑定显示缓冲 */
    disp_ = rgb;
    last_scale_ = QSize(); /* 缓存失效 */
  } else {
    rgb.copyTo(disp_);
  }
  /* 更新缩放缓存 */
  QRect vp = rect().adjusted(2, 2, -2, -2);
  QRect dr = imageDisplayRect(QSize(disp_.cols, disp_.rows), vp);
  if (dr.isEmpty()) return;
  QSize target(dr.width(), dr.height());
  if (target != last_scale_) {
    QImage img(disp_.data, disp_.cols, disp_.rows, (int)disp_.step,
               QImage::Format_RGB888);
    cached_pix_ = QPixmap::fromImage(img)
                      .scaled(target, Qt::IgnoreAspectRatio,
                              Qt::FastTransformation);
    last_scale_ = target;
  } else {
    QImage img(disp_.data, disp_.cols, disp_.rows, (int)disp_.step,
               QImage::Format_RGB888);
    QPainter p(&cached_pix_);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(cached_pix_.rect(), img.scaled(cached_pix_.size()));
  }
  drawn_rect_ = dr;
  syncLinesToPipeline(); /* 首帧/分辨率变化后立即同步竖线到 pipeline */
  update();
}

void VideoWidget::setLineFracs(double left, double right) {
  left_frac_ = std::clamp(left, 0.0, 1.0);
  right_frac_ = std::clamp(right, 0.0, 1.0);
  syncLinesToPipeline();
  update();
}

void VideoWidget::syncLinesToPipeline() {
  if (drawn_rect_.width() > 0) {
    /* pipeline 期望图像内坐标 [0, img_w],不能混入 letterbox 黑边偏移
     * (drawn_rect_.x()),否则宽屏下右线比例会 >1 被画出图外 */
    pipeline::g_view_width = drawn_rect_.width();
    pipeline::g_line_left_x.store((int)(left_frac_ * drawn_rect_.width()));
    pipeline::g_line_right_x.store((int)(right_frac_ * drawn_rect_.width()));
  }
}

void VideoWidget::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  if (cached_pix_.isNull()) {
    p.setPen(QColor(128, 128, 128));
    p.drawText(rect(), Qt::AlignCenter, QStringLiteral("无画面"));
    return;
  }
  p.drawPixmap(drawn_rect_, cached_pix_);

  /* 两条竖线(青/品红,与图像内 worker 画的颜色一致) */
  auto draw_line = [&](double frac, const QColor &c, const QString &tag) {
    int x = drawn_rect_.x() + (int)(frac * drawn_rect_.width());
    p.setPen(QPen(c, 3));
    p.drawLine(x, drawn_rect_.y(), x, drawn_rect_.bottom());
    /* 顶部抓手标签 */
    p.setBrush(c);
    QRect tagR(x - 9, drawn_rect_.y(), 18, 16);
    p.drawRect(tagR);
    p.setPen(Qt::black);
    p.drawText(tagR, Qt::AlignCenter, tag);
  };
  draw_line(left_frac_, QColor(0, 255, 255), "L");
  draw_line(right_frac_, QColor(255, 0, 255), "R");
}

void VideoWidget::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton || drawn_rect_.width() <= 0) return;
  int lx = drawn_rect_.x() + (int)(left_frac_ * drawn_rect_.width());
  int rx = drawn_rect_.x() + (int)(right_frac_ * drawn_rect_.width());
  int mx = e->pos().x();
  /* 距离近者优先,避免两线贴近时拖错 */
  int dl = std::abs(mx - lx), dr = std::abs(mx - rx);
  if (dl <= kHitRadius && dl <= dr)
    dragging_ = DRAG_LEFT;
  else if (dr <= kHitRadius)
    dragging_ = DRAG_RIGHT;
  if (dragging_ != DRAG_NONE) e->accept();
}

void VideoWidget::mouseMoveEvent(QMouseEvent *e) {
  if (dragging_ == DRAG_NONE) return;
  double frac =
      (double)(e->pos().x() - drawn_rect_.x()) / drawn_rect_.width();
  frac = std::clamp(frac, 0.0, 1.0);
  if (dragging_ == DRAG_LEFT)
    left_frac_ = frac;
  else
    right_frac_ = frac;
  syncLinesToPipeline();
  emit linesChanged(left_frac_, right_frac_);
  update();
}

void VideoWidget::mouseReleaseEvent(QMouseEvent *) {
  if (dragging_ != DRAG_NONE) emit dragFinished();
  dragging_ = DRAG_NONE;
}

void VideoWidget::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  last_scale_ = QSize(); /* 缓存失效,下帧重建 */
  QRect vp = rect().adjusted(2, 2, -2, -2);
  if (!disp_.empty())
    drawn_rect_ = imageDisplayRect(QSize(disp_.cols, disp_.rows), vp);
  syncLinesToPipeline();
}
