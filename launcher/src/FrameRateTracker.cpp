#include "FrameRateTracker.h"

#include <cmath>

void FrameRateTracker::reset() {
  mPreviousPresentedFrameTimestamp.reset();
  mPresentedFrameIntervals.clear();
  mSamples.clear();
  mPresentedFrameDurationSeconds = 0.0;
  mFramesPerSecond = 0.0;
}

void FrameRateTracker::recordPresentedFrame(double timestampSeconds) {
  if (!std::isfinite(timestampSeconds)) {
    return;
  }

  if (!mPreviousPresentedFrameTimestamp) {
    mPreviousPresentedFrameTimestamp = timestampSeconds;
    return;
  }

  auto const duration =
      timestampSeconds - *mPreviousPresentedFrameTimestamp;
  if (duration <= 0.0) {
    return;
  }
  mPreviousPresentedFrameTimestamp = timestampSeconds;

  mPresentedFrameIntervals.push_back({timestampSeconds, duration});
  mPresentedFrameDurationSeconds += duration;

  auto const calculationStart =
      timestampSeconds - CalculationWindowSeconds;
  while (!mPresentedFrameIntervals.empty() &&
         mPresentedFrameIntervals.front().endTimestampSeconds <
             calculationStart) {
    mPresentedFrameDurationSeconds -=
        mPresentedFrameIntervals.front().durationSeconds;
    mPresentedFrameIntervals.pop_front();
  }

  mFramesPerSecond =
      mPresentedFrameDurationSeconds > 0.0
          ? static_cast<double>(mPresentedFrameIntervals.size()) /
                mPresentedFrameDurationSeconds
          : 0.0;
  mSamples.push_back({timestampSeconds, mFramesPerSecond});

  auto const historyStart = timestampSeconds - HistoryWindowSeconds;
  while (!mSamples.empty() &&
         mSamples.front().timestampSeconds < historyStart) {
    mSamples.pop_front();
  }
}

double FrameRateTracker::framesPerSecond() const {
  return mFramesPerSecond;
}

std::deque<FrameRateSample> const& FrameRateTracker::samples() const {
  return mSamples;
}
