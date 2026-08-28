/* source/blender/io/usd/intern/usd_notice_handler.cc */
#include "usd_notice_handler.hh"

namespace blender::io::usd {

void UsdStageListener::register_listener(const pxr::UsdStageRefPtr &stage, const bContext *C)
{
  if (!stage)
    return;
  context_ = C;

  notice_key_ = pxr::TfNotice::Register(
      pxr::TfCreateWeakPtr(this), &UsdStageListener::on_objects_changed, stage);
}

void UsdStageListener::unregister_listener()
{
  if (notice_key_.IsValid()) {
    pxr::TfNotice::Revoke(notice_key_);
  }
}

void UsdStageListener::on_objects_changed(const pxr::UsdNotice::ObjectsChanged & /*notice*/,
                                          const pxr::UsdStageWeakPtr & /*sender*/)
{
  dirty_.store(true, std::memory_order_relaxed);
}

}  // namespace blender::io::usd
