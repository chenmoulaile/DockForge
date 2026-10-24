#include "shell/thumbnail_host.h"

#include "core/log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace dockforge::shell {
namespace {

constexpr char kLogModule[] = "thumb";

int DipToPx(float dip, float scale) {
    return static_cast<int>(lroundf(dip * scale));
}

}  // namespace

ThumbnailHost::~ThumbnailHost() { ReleaseAll(); }

int ThumbnailHost::SetSources(const std::vector<HWND>& sources) {
    // Drop null sources: a null HWND is not a window, and registering one would
    // either fail or, worse, resolve to the desktop.
    std::vector<HWND> wanted;
    wanted.reserve(sources.size());
    for (HWND source : sources) {
        if (source != nullptr) wanted.push_back(source);
    }
    // The measured render ceiling is 8 slots per destination (C-17c). Capping
    // here rather than in PumpPending means the pending queue can never hold work
    // that cannot be shown.
    if (static_cast<int>(wanted.size()) > kMaxThumbnails) {
        wanted.resize(static_cast<std::size_t>(kMaxThumbnails));
    }

    if (!dest_) {
        // No destination yet: everything is pending. The caller pumps again
        // after the popup exists.
        pending_ = wanted;
        return 0;
    }

    // Drop slots whose source is no longer wanted. Everything else keeps its
    // registration, which matters because re-registering is the expensive path.
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [&wanted](const ThumbnailSlot& slot) {
                                    return std::find(wanted.begin(), wanted.end(),
                                                     slot.source) == wanted.end();
                                }),
                 slots_.end());

    // Queue sources that have no live slot yet.
    pending_.clear();
    for (HWND source : wanted) {
        bool live = false;
        for (const ThumbnailSlot& slot : slots_) {
            if (slot.source == source) {
                live = true;
                break;
            }
        }
        if (!live) {
            pending_.push_back(source);
        }
    }

    // A source list that shrank leaves the tail slots unused; make sure the
    // geometry still matches the live count.
    UpdateDestinations(origin_x_px_, origin_y_px_, scale_);

    // Register what we can right away; the rest is pumped over later ticks.
    return PumpPending();
}

int ThumbnailHost::PumpPending() {
    int done = 0;
    while (done < kRegistrationsPerTick && !pending_.empty() &&
           static_cast<int>(slots_.size()) < kMaxThumbnails) {
        HWND source = pending_.front();

        // A missing destination or a dead source is not a failure to register:
        // it is work that cannot be done yet. Leave it queued so the next pump
        // retries, instead of dropping the slot forever.
        if (!dest_ || !IsWindow(source)) {
            DF_DEBUG(kLogModule, "defer source hwnd=%p dest=%p window=%d", (void*)source,
                     (void*)dest_, dest_ ? IsWindow(source) : FALSE);
            break;
        }
        pending_.erase(pending_.begin());

        ThumbnailSlot slot;
        slot.source = source;
        if (!RegisterSlot(slot, source, static_cast<int>(slots_.size()))) {
            continue;
        }
        slots_.push_back(slot);
        ++done;
    }
    return done;
}

bool ThumbnailHost::RegisterSlot(ThumbnailSlot& slot, HWND source, int index) {
    HRESULT hr = DwmRegisterThumbnail(dest_, source, &slot.thumb);
    if (FAILED(hr)) {
        DF_WARN(kLogModule, "DwmRegisterThumbnail hwnd=%p hr=0x%08lX", (void*)source, hr);
        return false;
    }

    SIZE source_size = {};
    hr = DwmQueryThumbnailSourceSize(slot.thumb, &source_size);
    if (FAILED(hr)) {
        DF_WARN(kLogModule, "DwmQueryThumbnailSourceSize hwnd=%p hr=0x%08lX", (void*)source, hr);
        DwmUnregisterThumbnail(slot.thumb);
        slot.thumb = nullptr;
        return false;
    }

    // Slot rect in destination client PHYSICAL pixels. `index` counts live slots
    // so a shrinking set leaves no hole.
    const int left = DipToPx(kSlotPaddingDip + index * (kSlotWidthDip + kSlotGapDip), scale_);
    const int top = DipToPx(kSlotPaddingDip, scale_);
    slot.dest.left = left;
    slot.dest.top = top;
    slot.dest.right = left + DipToPx(kSlotWidthDip, scale_);
    slot.dest.bottom = top + DipToPx(kSlotHeightDip, scale_);

    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
    props.rcDestination = slot.dest;
    props.opacity = 255;
    props.fVisible = TRUE;
    // The window frame belongs to the thumbnail as much as the client area does;
    // cropping to the client area made previews read as if they lost their title
    // bars in the spike.
    props.fSourceClientAreaOnly = FALSE;

    hr = DwmUpdateThumbnailProperties(slot.thumb, &props);
    if (FAILED(hr)) {
        DF_WARN(kLogModule, "DwmUpdateThumbnailProperties hwnd=%p hr=0x%08lX", (void*)source, hr);
        DwmUnregisterThumbnail(slot.thumb);
        slot.thumb = nullptr;
        return false;
    }

    DF_DEBUG(kLogModule, "registered hwnd=%p slot=%d src=%ldx%ld dest=%d,%d..%d,%d",
             (void*)source, index, (long)source_size.cx, (long)source_size.cy,
             slot.dest.left, slot.dest.top, slot.dest.right, slot.dest.bottom);
    return true;
}

void ThumbnailHost::ReleaseAll() {
    for (ThumbnailSlot& slot : slots_) {
        if (slot.thumb) {
            DwmUnregisterThumbnail(slot.thumb);
            slot.thumb = nullptr;
        }
        slot.source = nullptr;
    }
    slots_.clear();
    pending_.clear();
}

void ThumbnailHost::UpdateDestinations(int left_px, int top_px, float scale) {
    origin_x_px_ = left_px;
    origin_y_px_ = top_px;
    scale_ = scale;

    for (size_t i = 0; i < slots_.size(); ++i) {
        ThumbnailSlot& slot = slots_[i];
        const int left =
            DipToPx(kSlotPaddingDip + static_cast<float>(i) * (kSlotWidthDip + kSlotGapDip), scale);
        const int top = DipToPx(kSlotPaddingDip, scale);
        slot.dest.left = left;
        slot.dest.top = top;
        slot.dest.right = left + DipToPx(kSlotWidthDip, scale);
        slot.dest.bottom = top + DipToPx(kSlotHeightDip, scale);

        DWM_THUMBNAIL_PROPERTIES props = {};
        props.dwFlags = DWM_TNP_RECTDESTINATION;
        props.rcDestination = slot.dest;
        DwmUpdateThumbnailProperties(slot.thumb, &props);
    }
}

}  // namespace dockforge::shell
