#include "preview_provider.h"

#include <QtGlobal>

namespace rcapp {

PreviewProvider::PreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

void PreviewProvider::update(std::shared_ptr<const rcplatform::BgraPreviewFrame> frame) {
  if (!frame || frame->pixels.empty() || frame->width == 0 || frame->height == 0) return;
  const QImage view(frame->pixels.data(), static_cast<int>(frame->width),
                    static_cast<int>(frame->height), static_cast<qsizetype>(frame->stride),
                    QImage::Format_ARGB32);
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = view.copy();
}

QImage PreviewProvider::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

QImage PreviewProvider::requestImage(const QString&, QSize* size,
                                     const QSize& requestedSize) {
  QImage image;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    image = latest_;
  }
  if (size != nullptr) *size = image.size();
  if (!image.isNull() && requestedSize.isValid()) {
    return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

}  // namespace rcapp
