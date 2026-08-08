#include "render/StreamingBudget.hpp"

#include <cassert>

int main() {
    using mc::render::kMaxStreamingBudgetHigh;
    using mc::render::kMaxStreamingBudgetLow;
    using mc::render::streamingUploadBudgetForFrameMs;

    // GPU stressed (frame time above the stress threshold): the budget drops to
    // the low value no matter what it currently is.
    assert(streamingUploadBudgetForFrameMs(20.0F, kMaxStreamingBudgetHigh) ==
           kMaxStreamingBudgetLow);
    assert(streamingUploadBudgetForFrameMs(14.0F, kMaxStreamingBudgetHigh) ==
           kMaxStreamingBudgetLow);
    assert(streamingUploadBudgetForFrameMs(13.5F, kMaxStreamingBudgetHigh) ==
           kMaxStreamingBudgetLow);

    // GPU idle (frame time below the recovery threshold): the budget returns to
    // the high value so regions fill in fast again.
    assert(streamingUploadBudgetForFrameMs(9.0F, kMaxStreamingBudgetLow) ==
           kMaxStreamingBudgetHigh);
    assert(streamingUploadBudgetForFrameMs(5.0F, kMaxStreamingBudgetLow) ==
           kMaxStreamingBudgetHigh);
    assert(streamingUploadBudgetForFrameMs(9.9F, kMaxStreamingBudgetLow) ==
           kMaxStreamingBudgetHigh);

    // Inside the hysteresis band the current budget is kept, so a frame time
    // hovering around a single threshold cannot oscillate the budget.
    assert(streamingUploadBudgetForFrameMs(11.5F, kMaxStreamingBudgetLow) ==
           kMaxStreamingBudgetLow);
    assert(streamingUploadBudgetForFrameMs(11.5F, kMaxStreamingBudgetHigh) ==
           kMaxStreamingBudgetHigh);
    assert(streamingUploadBudgetForFrameMs(10.0F, 8U) == 8U);
    assert(streamingUploadBudgetForFrameMs(13.0F, 8U) == 8U);
    return 0;
}
