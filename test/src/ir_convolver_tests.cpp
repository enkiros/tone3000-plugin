// IrConvolver: the off-thread-tail IR convolver.
//
// Pins the properties the fix depends on:
//   - the short/long split matches the 1 s cutoff (cab = no worker, reverb =
//     head + worker tail);
//   - a long IR convolved through IrConvolver matches the reference
//     juce::dsp::Convolution (NonUniform) at every host block size, in mono and
//     true stereo — i.e. the split adds no latency, no gap and no overlap, and
//     only reorders float summation (so the bound is far above a real bug);
//   - a short IR stays on the plain zero-latency engine and matches exactly.
#include "IrConvolver.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

namespace {

constexpr int kShortIrMaxBaseSamples = 48000;   // 1 s, same cutoff as the loader
constexpr int kRefHeadSamples = 8192;           // matches kIrHeadSamples

std::unique_ptr<IrConvolver> makeSplit(const juce::File& file, juce::dsp::Convolution::Stereo stereo,
                                       int blockSize) {
  auto conv = std::make_unique<IrConvolver>();
  conv->loadImpulseResponse(file, stereo, juce::dsp::Convolution::Trim::yes, 0,
                            juce::dsp::Convolution::Normalise::no);
  conv->prepare({kFs, static_cast<juce::uint32>(blockSize), 2});
  conv->warmUp(0.15);
  return conv;
}

std::unique_ptr<juce::dsp::Convolution> makeReference(const juce::File& file,
                                                      juce::dsp::Convolution::Stereo stereo,
                                                      int blockSize, bool longIr) {
  auto conv = longIr
                  ? std::make_unique<juce::dsp::Convolution>(
                        juce::dsp::Convolution::NonUniform{kRefHeadSamples})
                  : std::make_unique<juce::dsp::Convolution>();
  conv->loadImpulseResponse(file, stereo, juce::dsp::Convolution::Trim::yes, 0,
                            juce::dsp::Convolution::Normalise::no);
  conv->prepare({kFs, static_cast<juce::uint32>(blockSize), 2});
  // Elapse JUCE's install crossfade on silence, like the split's warmUp().
  juce::AudioBuffer<float> silence(2, blockSize);
  for (int done = 0; done < static_cast<int>(kFs * 0.15); done += blockSize) {
    silence.clear();
    juce::dsp::AudioBlock<float> b(silence);
    conv->process(juce::dsp::ProcessContextReplacing<float>(b));
  }
  return conv;
}

// Processes `buffer` (already filled with the input) in `blockSize` chunks
// through the split convolver, pacing the worker so the tail it reads is
// always ready (the worker runs on a separate thread; without pacing a fast
// single-core test could out-run it). The worker consumes kIrTailBlockSamples
// at a time, so we wait for it to catch up at every 1024-sample boundary.
void runSplit(IrConvolver& conv, juce::AudioBuffer<float>& buffer, int blockSize) {
  const int total = buffer.getNumSamples();
  for (int off = 0; off < total; off += blockSize) {
    const int frames = std::min(blockSize, total - off);
    juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(), 2,
                                       static_cast<size_t>(off), static_cast<size_t>(frames));
    conv.process(juce::dsp::ProcessContextReplacing<float>(block));
    if ((off + frames) % kIrTailBlockSamples == 0)
      conv.waitForTail();
  }
}

void runReference(juce::dsp::Convolution& conv, juce::AudioBuffer<float>& buffer, int blockSize) {
  const int total = buffer.getNumSamples();
  for (int off = 0; off < total; off += blockSize) {
    const int frames = std::min(blockSize, total - off);
    juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(), 2,
                                       static_cast<size_t>(off), static_cast<size_t>(frames));
    conv.process(juce::dsp::ProcessContextReplacing<float>(block));
  }
}

// Maximum |split - reference| / peak over the settled region.
float maxRelativeDiff(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b,
                      int start) {
  float peak = 0.0f, maxDiff = 0.0f;
  for (int ch = 0; ch < 2; ++ch)
    for (int i = start; i < a.getNumSamples(); ++i) {
      peak = std::max(peak, std::abs(b.getSample(ch, i)));
      maxDiff = std::max(maxDiff, std::abs(a.getSample(ch, i) - b.getSample(ch, i)));
    }
  return peak > 0.0f ? maxDiff / peak : maxDiff;
}

TEST(IrConvolverTest, ShortIrUsesNoWorkerAndMatchesReference) {
  const int blockSize = 64;
  const int total = 48000;
  const auto noise = makeNoise(total, 1, 0.25f);

  auto split = makeSplit(testFile("cab-ir-test.wav"), juce::dsp::Convolution::Stereo::no, blockSize);
  auto ref = makeReference(testFile("cab-ir-test.wav"), juce::dsp::Convolution::Stereo::no,
                           blockSize, false);

  EXPECT_FALSE(split->isLong());
  EXPECT_EQ(split->getCurrentIRSize(), ref->getCurrentIRSize());
  EXPECT_LT(split->getCurrentIRSize(), kShortIrMaxBaseSamples);

  juce::AudioBuffer<float> a(2, total), b(2, total);
  a.copyFrom(0, 0, noise.data(), total);
  a.copyFrom(1, 0, noise.data(), total);
  b.copyFrom(0, 0, noise.data(), total);
  b.copyFrom(1, 0, noise.data(), total);

  runSplit(*split, a, blockSize);
  runReference(*ref, b, blockSize);

  EXPECT_LT(maxRelativeDiff(a, b, 0), 1e-4f);
}

TEST(IrConvolverTest, LongMonoIrMatchesReferenceAtEveryBlockSize) {
  const int total = 2 * 48000;
  const auto noise = makeNoise(total, 2, 0.25f);

  for (int blockSize : {64, 128, 256}) {
    auto split =
        makeSplit(testFile("reverb-ir-mono-test.wav"), juce::dsp::Convolution::Stereo::no, blockSize);
    auto ref = makeReference(testFile("reverb-ir-mono-test.wav"), juce::dsp::Convolution::Stereo::no,
                             blockSize, true);

    EXPECT_TRUE(split->isLong());
    EXPECT_GT(split->getCurrentIRSize(), kShortIrMaxBaseSamples);
    EXPECT_EQ(split->getCurrentIRSize(), ref->getCurrentIRSize());

    juce::AudioBuffer<float> a(2, total), b(2, total);
    a.copyFrom(0, 0, noise.data(), total);
    a.copyFrom(1, 0, noise.data(), total);
    b.copyFrom(0, 0, noise.data(), total);
    b.copyFrom(1, 0, noise.data(), total);

    runSplit(*split, a, blockSize);
    runReference(*ref, b, blockSize);

    EXPECT_LT(maxRelativeDiff(a, b, 0), 1e-4f)
        << "mono reverb at block " << blockSize << " deviates from the reference";
  }
}

TEST(IrConvolverTest, LongStereoIrMatchesReference) {
  const int total = 2 * 48000;
  const auto noise = makeNoise(total, 3, 0.25f);

  for (int blockSize : {64, 256}) {
    auto split = makeSplit(testFile("reverb-ir-stereo-test.wav"),
                           juce::dsp::Convolution::Stereo::yes, blockSize);
    auto ref = makeReference(testFile("reverb-ir-stereo-test.wav"),
                             juce::dsp::Convolution::Stereo::yes, blockSize, true);

    EXPECT_TRUE(split->isLong());

    juce::AudioBuffer<float> a(2, total), b(2, total);
    a.copyFrom(0, 0, noise.data(), total);
    a.copyFrom(1, 0, noise.data(), total);
    b.copyFrom(0, 0, noise.data(), total);
    b.copyFrom(1, 0, noise.data(), total);

    runSplit(*split, a, blockSize);
    runReference(*ref, b, blockSize);

    EXPECT_LT(maxRelativeDiff(a, b, 0), 1e-4f)
        << "stereo reverb at block " << blockSize << " deviates from the reference";
  }
}

TEST(IrConvolverTest, LongIrHasNoAddedLatency) {
  // An impulse through the split must start exactly where the reference does:
  // the tail is delayed by exactly kIrHeadSamples and the head covers that gap,
  // so there is no net latency. Compare the full impulse responses.
  const int blockSize = 64;
  const int total = 2 * 48000;
  std::vector<float> impulse(static_cast<size_t>(total), 0.0f);
  impulse[0] = 1.0f;

  auto split =
      makeSplit(testFile("reverb-ir-mono-test.wav"), juce::dsp::Convolution::Stereo::no, blockSize);
  auto ref = makeReference(testFile("reverb-ir-mono-test.wav"), juce::dsp::Convolution::Stereo::no,
                           blockSize, true);

  juce::AudioBuffer<float> a(2, total), b(2, total);
  a.copyFrom(0, 0, impulse.data(), total);
  a.copyFrom(1, 0, impulse.data(), total);
  b.copyFrom(0, 0, impulse.data(), total);
  b.copyFrom(1, 0, impulse.data(), total);

  runSplit(*split, a, blockSize);
  runReference(*ref, b, blockSize);

  // The impulse response must be non-silent and aligned with the reference
  // (an added latency or a misaligned tail shows up as a large per-sample diff).
  float peak = 0.0f;
  for (int i = 0; i < total; ++i)
    peak = std::max(peak, std::abs(b.getSample(0, i)));
  ASSERT_GT(peak, 1e-3f) << "reference impulse response is silent";
  EXPECT_LT(maxRelativeDiff(a, b, 0), 1e-4f)
      << "impulse response of the split deviates from the reference";
}

}  // namespace
