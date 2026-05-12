/* source/blender/io/usd/intern/usd_notice_handler.hh */
#pragma once

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

 private:
  /* Callback triggered by Pixar USD when the file changes on disk. */
  void on_objects_changed(const pxr::UsdNotice::ObjectsChanged &notice,
                          const pxr::UsdStageWeakPtr &sender);

  pxr::TfNotice::Key notice_key_;
  const bContext *context_ = nullptr;
};

}  // namespace blender::io::usd
