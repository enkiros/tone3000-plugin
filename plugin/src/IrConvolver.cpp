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

// Bounded wait for the tail worker (see IrConvolver::process). In real time
// the worker stays ahead of the audio thread, so this is never entered; it
// only bounds a fast-driven render (tests) or a dead worker to silence.
inline constexpr int kMaxTailWaitSpins = 1'000'000;

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
        owner.workerEvent.wait(50);
        owner.workerEvent.reset();  // park until the next signal, don't spin
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
  // adding zero there is a no-op. See the spin-wait in process().
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
  ensureRings();
  if (tailConvolver != nullptr)
    startWorker();
}

void IrConvolver::process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept {
  if (headConvolver == nullptr)
    return;

  auto& block = context.getOutputBlock();
  const int blockChannels = static_cast<int>(block.getNumChannels());
  const int numSamples = static_cast<int>(block.getNumSamples());

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
    if (inputWrite.load(std::memory_order_acquire) - inputRead.load(std::memory_order_acquire) >=
        kIrTailBlockSamples)
      workerEvent.signal();
  }

  headConvolver->process(context);

  if (tailConvolver != nullptr) {
    // Add the delayed tail output, channel for channel.
    const int64_t need = outStart + numSamples;
    // The tail for this block must already be produced. In real time the
    // worker stays ~kIrHeadSamples ahead of the audio thread, so this holds
    // immediately; the (bounded) wait only engages when the audio thread is
    // driven faster than the worker can keep up — off-line renders and the
    // test suite. A dead worker degrades to silence here, never a wedge.
    int spins = 0;
    while (tailAvailable.load(std::memory_order_acquire) < need && spins < kMaxTailWaitSpins) {
      juce::Thread::yield();
      ++spins;
    }
    const int64_t avail = tailAvailable.load(std::memory_order_acquire);
    const int64_t have =
        juce::jmax<int64_t>(0, juce::jmin<int64_t>(avail, need) - outStart);
    const int mask = kIrTailRingSamples - 1;
    for (int i = 0; i < have; ++i) {
      const int pos = static_cast<int>((outStart + i) & mask);
      for (int c = 0; c < blockChannels; ++c)
        block.getChannelPointer(c)[i] += outputRing.getReadPointer(c)[pos];
    }
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
  const int64_t target = inputWrite.load(std::memory_order_acquire);
  for (int i = 0; i < 100000; ++i) {
    if (inputRead.load(std::memory_order_acquire) >= target)
      return;
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
  if (!worker->isThreadRunning())
    worker->startThread(juce::Thread::Priority::highest);
}

void IrConvolver::stopWorker() {
  if (worker != nullptr && worker->isThreadRunning()) {
    workerEvent.signal();
    worker->stopThread(5000);
    workerEvent.reset();
  }
}
