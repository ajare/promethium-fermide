#pragma once

#include <deque>
#include <optional>

struct FrameRateSample {
  double timestampSeconds{0.0};
  double framesPerSecond{0.0};
};

// Measures completed presentation intervals. The displayed rate covers the
// most recent second, while the graph retains five seconds of calculated rates.
class FrameRateTracker {
  struct PresentedFrameInterval {
    double endTimestampSeconds;
    double durationSeconds;
  };

  static constexpr double CalculationWindowSeconds = 1.0;
  static constexpr double HistoryWindowSeconds = 5.0;

  std::optional<double> mPreviousPresentedFrameTimestamp;
  std::deque<PresentedFrameInterval> mPresentedFrameIntervals;
  std::deque<FrameRateSample> mSamples;
  double mPresentedFrameDurationSeconds{0.0};
  double mFramesPerSecond{0.0};

public:
  void reset();
  void recordPresentedFrame(double timestampSeconds);

  [[nodiscard]] double framesPerSecond() const;
  [[nodiscard]] std::deque<FrameRateSample> const& samples() const;
};
