#pragma once
#include <algorithm>
#include <cstddef>
#include <cmath>
namespace ReplayBufferPro {
inline constexpr std::size_t MAX_INFLIGHT_CLIP_REQUESTS = 3;
inline constexpr bool canAcceptClipRequest(std::size_t inflight) noexcept {
 return inflight < MAX_INFLIGHT_CLIP_REQUESTS;
}
// Allow at most 50 ms of timestamp/container rounding, not seconds of missing history.
inline bool hasFullClipDuration(double actualSeconds, int requestedSeconds) noexcept {
 return requestedSeconds > 0 && std::isfinite(actualSeconds) &&
        actualSeconds + 0.05 >= static_cast<double>(requestedSeconds);
}
// Preserve the original stable ordering for equal DTS values.
template<class It, class Compare>
void stableSortIfNeeded(It begin, It end, Compare less) {
 if (!std::is_sorted(begin, end, less)) std::stable_sort(begin, end, less);
}
}
