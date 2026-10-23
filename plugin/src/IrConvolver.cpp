#include "IrConvolver.h"

#include "ChainDomain.h"  // kChainBaseSampleRate

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstring>

namespace {

// The short/long cutoff, in base-rate samples. Must match
// kShortIrMaxBaseSamples in ProcessorModelLoader.cpp (the 1 s reverb
// convention); both drive the same engine split.
inline constexpr int kIrShortMaxBaseSamples = 48000;

// --- IR loading: the same read → trim → resample → gain pipeline
// juce::dsp::Convolution runs internally, reproduced here so the kernel can
// be split at kIrHeadSamples. Faithful to the pinned JUCE implementation
// (juce_Convolution.cpp: loadStreamToBuffer / fixNumChannels /
// trimImpulseResponse / resampleImpulseResponse / normaliseImpulseResponse).

juce::AudioBuffer<float> fixNumChannels(const juce::AudioBuffer<float>& buf,
                                        juce::dsp::Convolution::Stereo stereo) {
  const int numChannels =
      juce::jmin(buf.getNumChannels(), stereo == juce::dsp::Convolution::Stereo::yes ? 2 : 1);
  juce::AudioBuffer<float> result(numChannels, buf.getNumSamples());
  for (int c = 0; c < numChannels; ++c)
    result.copyFrom(c, 0, buf.getReadPointer(c), buf.getNumSamples());
  if (result.getNumSamples() == 0 || result.getNumChannels() == 0) {
    result.setSize(1, 1);
    result.setSample(0, 0, 1.0f);
  }
  return result;
}

juce::AudioBuffer<float> trimImpulseResponse(const juce::AudioBuffer<float>& buf) {
  const float thresholdTrim = juce::Decibels::decibelsToGain(-80.0f);
  const int numChannels = buf.getNumChannels();
  const int numSamples = buf.getNumSamples();

  std::ptrdiff_t offsetBegin = numSamples;
  std::ptrdiff_t offsetEnd = numSamples;

  for (int channel = 0; channel < numChannels; ++channel) {
    const auto indexAboveThreshold = [&](auto begin, auto end) {
      return std::distance(begin, std::find_if(begin, end, [&](float sample) {
                               return std::abs(sample) >= thresholdTrim;
                             }));
    };
    const auto* channelBegin = buf.getReadPointer(channel);
    const auto* channelEnd = channelBegin + numSamples;
    const auto itStart = indexAboveThreshold(channelBegin, channelEnd);
    const auto itEnd = indexAboveThreshold(std::make_reverse_iterator(channelEnd),
                                           std::make_reverse_iterator(channelBegin));
    offsetBegin = juce::jmin(offsetBegin, itStart);
    offsetEnd = juce::jmin(offsetEnd, itEnd);
  }

  if (offsetBegin == numSamples) {
    juce::AudioBuffer<float> result(numChannels, 1);
    result.clear();
    return result;
  }

  const int newLength = juce::jmax(1, numSamples - static_cast<int>(offsetBegin + offsetEnd));
  juce::AudioBuffer<float> result(numChannels, newLength);
  for (int channel = 0; channel < numChannels; ++channel)
    result.copyFrom(channel, 0, buf.getReadPointer(channel, static_cast<int>(offsetBegin)),
                    result.getNumSamples());
  return result;
}

juce::AudioBuffer<float> resampleImpulseResponse(const juce::AudioBuffer<float>& buf,
                                                 double srcSampleRate, double destSampleRate) {
  if (juce::approximatelyEqual(srcSampleRate, destSampleRate))
    return buf;

  const double factorReading = srcSampleRate / destSampleRate;
  juce::AudioBuffer<float> original = buf;
  juce::MemoryAudioSource memorySource(original, false);
  juce::ResamplingAudioSource resamplingSource(&memorySource, false, buf.getNumChannels());

  const int finalSize = juce::roundToInt(juce::jmax(1.0, buf.getNumSamples() / factorReading));
  resamplingSource.setResamplingRatio(factorReading);
  resamplingSource.prepareToPlay(finalSize, srcSampleRate);

  juce::AudioBuffer<float> result(buf.getNumChannels(), finalSize);
  resamplingSource.getNextAudioBlock({&result, 0, result.getNumSamples()});
  return result;
}

void normaliseImpulseResponse(juce::AudioBuffer<float>& buf) {
  const int numChannels = buf.getNumChannels();
  const int numSamples = buf.getNumSamples();
  const auto channelPtrs = buf.getArrayOfWritePointers();

  float maxSumSquaredMag = 0.0f;
  for (int c = 0; c < numChannels; ++c) {
    float sumSquared = 0.0f;
    const float* data = channelPtrs[c];
    for (int i = 0; i < numSamples; ++i)
      sumSquared += data[i] * data[i];
    maxSumSquaredMag = juce::jmax(maxSumSquaredMag, sumSquared);
  }

  const float factor =
      (maxSumSquaredMag < 1e-8f) ? 1.0f : 0.125f / std::sqrt(maxSumSquaredMag);
  for (int c = 0; c < numChannels; ++c)
    juce::FloatVectorOperations::multiply(channelPtrs[c], factor, numSamples);
}

juce::AudioBuffer<float> takeRange(const juce::AudioBuffer<float>& src, int start, int length) {
  juce::AudioBuffer<float> dst(src.getNumChannels(), length);
  for (int c = 0; c < src.getNumChannels(); ++c)
    dst.copyFrom(c, 0, src.getReadPointer(c) + start, length);
  return dst;
}

// Ring copy helpers. Ring length is a power of two; start may wrap.
void ringRead(const float* ring, int ringSize, int64_t start, float* dst, int n) {
  const int mask = ringSize - 1;
  const int p = static_cast<int>(start & mask);
  const int first = juce::jmin(n, ringSize - p);
  juce::FloatVectorOperations::copy(dst, ring + p, first);
  if (first < n)
    juce::FloatVectorOperations::copy(dst + first, ring, n - first);
}

void ringWrite(float* ring, int ringSize, int64_t start, const float* src, int n) {
  const int mask = ringSize - 1;
  const int p = static_cast<int>(start & mask);
  const int first = juce::jmin(n, ringSize - p);
  juce::FloatVectorOperations::copy(ring + p, src, first);
  if (first < n)
    juce::FloatVectorOperations::copy(ring, src + first, n - first);
}

}  // namespace

// ---------------------------------------------------------------------------

class IrConvolver::TailWorker : public juce::Thread {
public:
  explicit TailWorker(IrConvolver& o) : juce::Thread("IrTailWorker"), owner(o) {}

  void run() override {
    // Denormals are per-thread CPU state; a reverb tail decays into denormal
    // range and would cost ~100x on this thread without the flush (same
    // precaution the RtWorkerPool workers take).
    juce::ScopedNoDenormals noDenormals;

    const int kc = owner.numChannels;
    juce::AudioBuffer<float> localBuffer(kc, kIrTailBlockSamples);
    localBuffer.clear();

    while (!threadShouldExit()) {
      bool didWork = false;
      while (true) {
        const int64_t inW = owner.inputWrite.load(std::memory_order_acquire);
        const int64_t inR = owner.inputRead.load(std::memory_order_relaxed);
        if (inW - inR < kIrTailBlockSamples)
          break;

        // Test hook: simulate a descheduled worker (see setWorkerStallForTest).
        if (const int stall = owner.workerStallMs.load(std::memory_order_relaxed); stall > 0)
          juce::Thread::sleep(stall);

        for (int c = 0; c < kc; ++c)
          ringRead(owner.inputRing.getReadPointer(c), kIrTailRingSamples, inR,
                   localBuffer.getWritePointer(c), kIrTailBlockSamples);

        juce::dsp::AudioBlock<float> block(localBuffer.getArrayOfWritePointers(),
                                           static_cast<size_t>(kc),
                                           static_cast<size_t>(kIrTailBlockSamples));
        owner.tailConvolver->process(juce::dsp::ProcessContextReplacing<float>(block));

        // The tail output for input [inR, inR + kIrTailBlockSamples) lands at
        // output [inR + kIrHeadSamples, ...), exactly where the head ends.
        for (int c = 0; c < kc; ++c)
          ringWrite(owner.outputRing.getWritePointer(c), kIrTailRingSamples,
                    inR + kIrHeadSamples, localBuffer.getReadPointer(c), kIrTailBlockSamples);

        owner.inputRead.store(inR + kIrTailBlockSamples, std::memory_order_release);
        owner.tailAvailable.store(inR + kIrTailBlockSamples + kIrHeadSamples,
                                  std::memory_order_release);
        didWork = true;
      }
      if (!didWork) {
        // Poll instead of sleeping on a signal: the audio thread must never
        // touch a wait primitive (a WaitableEvent signal takes a pthread
        // mutex). The poll is ~240 samples at 48 kHz, far inside the worker's
        // ~165 ms of slack. stopWorker() still signals this event to wake us
        // for shutdown from a non-RT thread.
        owner.workerEvent.wait(kIrWorkerPollMs);
        owner.workerEvent.reset();  // park until the next poll or shutdown
      }
    }
  }

private:
  IrConvolver& owner;
};

// ---------------------------------------------------------------------------

IrConvolver::IrConvolver() {
  // The first kIrHeadSamples of output have no tail (the tail IR starts at
  // that offset), so they are "available" immediately; the ring is zeroed and
  // adding zero there is a no-op. See the fade margin in process().
  tailAvailable.store(kIrHeadSamples, std::memory_order_relaxed);
}

IrConvolver::~IrConvolver() { stopWorker(); }

void IrConvolver::loadImpulseResponse(const juce::File& file,
                                      juce::dsp::Convolution::Stereo stereo,
                                      juce::dsp::Convolution::Trim trim, size_t size,
                                      juce::dsp::Convolution::Normalise normalise) {
  stopWorker();

  juce::AudioFormatManager manager;
  manager.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(manager.createReaderFor(file));
  if (reader == nullptr) {
    headConvolver.reset();
    tailConvolver.reset();
    irSize = 0;
    longIr = false;
    return;
  }

  const auto fileLength = static_cast<size_t>(reader->lengthInSamples);
  const auto lengthToLoad = (size == 0) ? fileLength : juce::jmin(size, fileLength);
  const double fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : kChainBaseSampleRate;

  juce::AudioBuffer<float> buf(juce::jlimit(1, 2, static_cast<int>(reader->numChannels)),
                               static_cast<int>(lengthToLoad));
  reader->read(buf.getArrayOfWritePointers(), buf.getNumChannels(), 0, buf.getNumSamples());

  buf = fixNumChannels(buf, stereo);
  if (trim == juce::dsp::Convolution::Trim::yes)
    buf = trimImpulseResponse(buf);
  buf = resampleImpulseResponse(buf, fileRate, kChainBaseSampleRate);
  if (normalise == juce::dsp::Convolution::Normalise::yes)
    normaliseImpulseResponse(buf);
  else
    buf.applyGain(static_cast<float>(fileRate / kChainBaseSampleRate));

  irSize = buf.getNumSamples();
  longIr = irSize > kIrShortMaxBaseSamples;

  headConvolver = std::make_unique<juce::dsp::Convolution>();
  tailConvolver.reset();

  if (longIr && irSize > kIrHeadSamples) {
    tailConvolver = std::make_unique<juce::dsp::Convolution>(
        juce::dsp::Convolution::NonUniform{kIrHeadSamples});
    headConvolver->loadImpulseResponse(takeRange(buf, 0, kIrHeadSamples), kChainBaseSampleRate,
                                       stereo, juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::no);
    tailConvolver->loadImpulseResponse(takeRange(buf, kIrHeadSamples, irSize - kIrHeadSamples),
                                       kChainBaseSampleRate, stereo,
                                       juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::no);
  } else {
    longIr = false;
    headConvolver->loadImpulseResponse(std::move(buf), kChainBaseSampleRate, stereo,
                                       juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::no);
  }
}

void IrConvolver::prepare(const juce::dsp::ProcessSpec& spec) {
  stopWorker();

  if (headConvolver == nullptr)
    return;

  hostBlock = juce::jmax(1, static_cast<int>(spec.maximumBlockSize));
  numChannels = juce::jmin(2, static_cast<int>(spec.numChannels));
  const juce::uint32 specChannels = static_cast<juce::uint32>(numChannels);

  headConvolver->prepare({kChainBaseSampleRate, static_cast<juce::uint32>(hostBlock), specChannels});
  if (tailConvolver != nullptr)
    tailConvolver->prepare({kChainBaseSampleRate, static_cast<juce::uint32>(kIrTailBlockSamples),
                            specChannels});

  inputWrite.store(0, std::memory_order_release);
  inputRead.store(0, std::memory_order_release);
  tailAvailable.store(kIrHeadSamples, std::memory_order_release);
  resetUnderrunState();
  ensureRings();

  if (tailConvolver != nullptr)
    startWorker();
}

void IrConvolver::reset() {
  stopWorker();
  if (headConvolver == nullptr)
    return;
  headConvolver->reset();
  if (tailConvolver != nullptr)
    tailConvolver->reset();
  inputWrite.store(0, std::memory_order_release);
  inputRead.store(0, std::memory_order_release);
  tailAvailable.store(kIrHeadSamples, std::memory_order_release);
  resetUnderrunState();
  ensureRings();
  if (tailConvolver != nullptr)
    startWorker();
}

void IrConvolver::resetUnderrunState() {
  tailUnderrunCount.store(0, std::memory_order_relaxed);
  maxMissedSamples.store(0, std::memory_order_relaxed);
  tailGain = 1.0f;
  tailFadeRemaining = 0;
  tailFadeStepDown = 0.0f;
  tailFadeSamples = juce::jmax(1, juce::roundToInt(kIrTailFadeSeconds * kChainBaseSampleRate));
  tailFadeStepUp = 1.0f / static_cast<float>(tailFadeSamples);
}

void IrConvolver::process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept {
  if (headConvolver == nullptr)
    return;

  auto& block = context.getOutputBlock();
  const int blockChannels = static_cast<int>(block.getNumChannels());
  const int numSamples = static_cast<int>(block.getNumSamples());
  if (numSamples <= 0)
    return;

  int64_t outStart = 0;
  if (tailConvolver != nullptr) {
    outStart = inputWrite.load(std::memory_order_relaxed);

    // Feed the worker the dry input before the head convolver overwrites it.
    // Every input channel is copied; the tail convolver applies its kernel
    // (mono to each channel, or true stereo) exactly like the head does.
    const int kc = juce::jmin(numChannels, blockChannels);
    for (int c = 0; c < kc; ++c)
      ringWrite(inputRing.getWritePointer(c), kIrTailRingSamples, outStart,
                block.getChannelPointer(c), numSamples);

    inputWrite.store(outStart + numSamples, std::memory_order_release);
    // No signal to the worker here: a WaitableEvent signal takes a pthread
    // mutex on the audio thread. The worker polls its ring instead.
  }

  headConvolver->process(context);

  if (tailConvolver == nullptr)
    return;

  const int64_t need = outStart + numSamples;

  // Offline renders are driven faster than real time, so let the worker catch
  // up (bounded) to keep the bounce sample-exact. Wait for the healthy margin,
  // not just this block's need: the fade starts `tailFadeSamples` early, so
  // waiting for `need` alone would still trip it during a fast render. Real-time
  // hosts never take this path: waiting on the audio thread is what turned a
  // late worker into a callback overrun.
  const int64_t healthy =
      need + static_cast<int64_t>(tailFadeSamples);
  if (nonRealtime.load(std::memory_order_relaxed) &&
      tailAvailable.load(std::memory_order_acquire) < healthy && worker != nullptr &&
      worker->isThreadRunning()) {
    const auto deadline = juce::Time::getMillisecondCounter() + kIrOfflineWaitMs;
    while (tailAvailable.load(std::memory_order_acquire) < healthy &&
           juce::Time::getMillisecondCounter() < deadline)
      juce::Thread::sleep(1);
  }

  const int64_t avail = tailAvailable.load(std::memory_order_acquire);
  const int64_t have =
      juce::jlimit<int64_t>(0, static_cast<int64_t>(numSamples), avail - outStart);
  const int haveInt = static_cast<int>(have);
  const int64_t remaining = avail - outStart;

  if (haveInt < numSamples) {
    // The worker was late. Bounded strictly for diagnostics: the audio thread
    // is the only writer of these two, so plain relaxed RMWs are enough.
    tailUnderrunCount.fetch_add(1, std::memory_order_relaxed);
    if (const int missed = numSamples - haveInt;
        missed > maxMissedSamples.load(std::memory_order_relaxed))
      maxMissedSamples.store(missed, std::memory_order_relaxed);
  }

  // Fade the tail out while there is still enough of it in the ring to reach
  // zero before the data runs out, then fade back in once the worker is ahead
  // again. Healthy means the whole block plus the fade margin is present; the
  // margin guarantees the previous block had at least `tailFadeSamples` of
  // tail left when the fade began (see kIrTailFadeSeconds).
  const bool endangered =
      remaining < static_cast<int64_t>(numSamples) + static_cast<int64_t>(tailFadeSamples);
  if (endangered) {
    if (tailFadeRemaining == 0 && tailGain > 0.0f) {
      const int fadeN = static_cast<int>(juce::jlimit<int64_t>(
          1, static_cast<int64_t>(tailFadeSamples), juce::jmax<int64_t>(1, remaining)));
      tailFadeRemaining = fadeN;
      tailFadeStepDown = tailGain / static_cast<float>(fadeN);
    }
  } else {
    tailFadeRemaining = 0;
  }

  const int addChannels = juce::jmin(numChannels, blockChannels);
  const int mask = kIrTailRingSamples - 1;

  // Common path: worker ahead, tail fully wet. Vector-style add, no ramp.
  if (!endangered && tailGain >= 1.0f && haveInt == numSamples) {
    for (int i = 0; i < numSamples; ++i) {
      const int pos = static_cast<int>((outStart + i) & mask);
      for (int c = 0; c < addChannels; ++c)
        block.getChannelPointer(c)[i] += outputRing.getReadPointer(c)[pos];
    }
    return;
  }

  // Fading (in or out): advance the gain on every sample, including the ones
  // past `have`, so a fade-out completes even when the ring data runs out
  // mid-block. The missing samples are skipped; the ramp is what keeps the
  // seam inaudible. While the tail is endangered, hold at zero after the
  // fade-out rather than ramping back in mid-block.
  for (int i = 0; i < numSamples; ++i) {
    if (endangered) {
      if (tailFadeRemaining > 0) {
        tailGain = juce::jmax(0.0f, tailGain - tailFadeStepDown);
        if (--tailFadeRemaining == 0 || tailGain <= 0.0f) {
          tailGain = 0.0f;
          tailFadeRemaining = 0;
        }
      }
    } else if (tailGain < 1.0f) {
      tailGain = juce::jmin(1.0f, tailGain + tailFadeStepUp);
    }

    if (i >= haveInt)
      continue;
    const int pos = static_cast<int>((outStart + i) & mask);
    for (int c = 0; c < addChannels; ++c)
      block.getChannelPointer(c)[i] += outputRing.getReadPointer(c)[pos] * tailGain;
  }
}

void IrConvolver::warmUp(double seconds) {
  if (headConvolver == nullptr)
    return;
  const int headSamples = static_cast<int>(seconds * kChainBaseSampleRate);
  const int headBlock = juce::jmax(1, hostBlock);

  juce::AudioBuffer<float> headSilence(2, headBlock);
  headSilence.clear();
  for (int done = 0; done < headSamples; done += headBlock) {
    juce::dsp::AudioBlock<float> b(headSilence);
    headConvolver->process(juce::dsp::ProcessContextReplacing<float>(b));
  }

  if (tailConvolver != nullptr) {
    juce::AudioBuffer<float> tailSilence(numChannels, kIrTailBlockSamples);
    tailSilence.clear();
    for (int done = 0; done < headSamples; done += kIrTailBlockSamples) {
      juce::dsp::AudioBlock<float> b(tailSilence);
      tailConvolver->process(juce::dsp::ProcessContextReplacing<float>(b));
    }
  }
}

void IrConvolver::waitForTail() {
  if (tailConvolver == nullptr)
    return;
  // Wait for the worker's *published* output, not just inputRead: tailAvailable
  // is stored after the ring write and after inputRead, so this is the value
  // process() will see. The worker consumes whole kIrTailBlockSamples blocks,
  // so target the last block it can actually have consumed.
  const int64_t pushed = inputWrite.load(std::memory_order_acquire);
  const int64_t consumable = (pushed / kIrTailBlockSamples) * kIrTailBlockSamples;
  if (consumable <= 0)
    return;
  const int64_t target = consumable + kIrHeadSamples;
  for (int i = 0; i < 100000; ++i) {
    if (tailAvailable.load(std::memory_order_acquire) >= target)
      return;
    // Non-RT caller (tests/loader): wake the polling worker immediately so a
    // paced comparison does not pay the poll interval per block. The audio
    // thread must never do this; see process().
    workerEvent.signal();
    juce::Thread::sleep(1);
  }
}

void IrConvolver::ensureRings() {
  if (inputRing.getNumChannels() != numChannels || inputRing.getNumSamples() != kIrTailRingSamples)
    inputRing.setSize(numChannels, kIrTailRingSamples);
  inputRing.clear();
  if (outputRing.getNumChannels() != numChannels || outputRing.getNumSamples() != kIrTailRingSamples)
    outputRing.setSize(numChannels, kIrTailRingSamples);
  outputRing.clear();
}

void IrConvolver::startWorker() {
  if (worker == nullptr)
    worker = std::make_unique<TailWorker>(*this);
  if (worker->isThreadRunning())
    return;

  // Realtime where the platform allows it. On Linux without rtprio privileges
  // (the Pi kiosk's normal case) the realtime start is refused and this falls
  // back to a plain thread, just like RtWorkerPool. The tail worker is not
  // hard-realtime: the fade in process() keeps a late wake-up inaudible.
  const double frameMs = 1000.0 * kIrHeadSamples / kChainBaseSampleRate;
  const double budgetMs = juce::jlimit(0.1, 40.0, frameMs);
  const bool startedRealtime = worker->startRealtimeThread(
      juce::Thread::RealtimeOptions{}.withPriority(10).withMaximumProcessingTimeMs(budgetMs));
  if (!startedRealtime)
    worker->startThread(juce::Thread::Priority::highest);
}

void IrConvolver::stopWorker() {
  if (worker != nullptr && worker->isThreadRunning()) {
    workerEvent.signal();
    worker->stopThread(5000);
    workerEvent.reset();
  }
}
