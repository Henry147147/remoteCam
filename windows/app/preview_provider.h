#ifndef RCAPP_PREVIEW_PROVIDER_H
#define RCAPP_PREVIEW_PROVIDER_H

#include <QImage>
#include <QQuickImageProvider>

#include <memory>
#include <mutex>

#include "rcplatform/frame_ring_sink.h"

namespace rcapp {

// Thread-safe CPU preview fallback fed by the already-required asynchronous staging
// readback. It keeps only the newest image; QML never waits for the media worker.
class PreviewProvider final : public QQuickImageProvider {
 public:
  PreviewProvider();

  void update(std::shared_ptr<const rcplatform::BgraPreviewFrame> frame);
  QImage snapshot() const;
  QImage requestImage(const QString& id, QSize* size,
                      const QSize& requestedSize) override;

 private:
  mutable std::mutex mutex_;
  QImage latest_;
};

}  // namespace rcapp

#endif  // RCAPP_PREVIEW_PROVIDER_H
