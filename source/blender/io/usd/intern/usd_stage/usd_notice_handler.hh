/* source/blender/io/usd/intern/usd_notice_handler.hh */
#pragma once

#include <atomic>

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/usd/notice.h>

namespace blender {
struct bContext;
}

namespace blender::io::usd {

class UsdStageListener : public pxr::TfWeakBase {
 public:
  /* Bind this listener to a specific stage. */
  void register_listener(const pxr::UsdStageRefPtr &stage, const bContext *C);

  /* Unregister to prevent crashes on stage close. */
  void unregister_listener();

  /* Set when the stage changes externally (e.g. Unreal Live Link write).
   * Thread-safe: the notice callback may fire from a non-main thread. */
  bool is_dirty() const
  {
    return dirty_.load(std::memory_order_relaxed);
  }
  void clear_dirty()
  {
    dirty_.store(false, std::memory_order_relaxed);
  }

 private:
  /* Callback triggered by Pixar USD when the file changes on disk. */
  void on_objects_changed(const pxr::UsdNotice::ObjectsChanged &notice,
                          const pxr::UsdStageWeakPtr &sender);

  pxr::TfNotice::Key notice_key_;
  const bContext *context_ = nullptr;
  std::atomic<bool> dirty_{false};
};

}  // namespace blender::io::usd
