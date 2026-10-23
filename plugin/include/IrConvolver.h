#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <cstdint>
#include <memory>

// Off-thread-tail IR convolver.
//
// juce::dsp::Convolution's two-stage NonUniform engine keeps the first
// `kIrHeadSamples` samples (the "head") on the audio thread and computes the
// remaining tail as a single FFT batch every `kIrHeadSamples` input samples.
// That batch is ~2 x (tail length) complex MACs in one callback, which at a
// 64/128-sample host block misses the deadline and pops (it fits 256). This
// class splits the tail off to a worker thread so the spike never lands on the
// audio thread, without adding latency or truncating the IR:
//
//   - head (first kIrHeadSamples): a zero-latency juce::dsp::Convolution,
//     processed on the audio thread exactly as today;
//   - tail (the rest): a second juce::dsp::Convolution (still NonUniform)
//     run by a worker on kIrTailBlockSamples-sample blocks. Its output is
//     added kIrHeadSamples samples later, precisely where the head ends, so
//     the whole thing is the same zero-latency convolution, just partitioned
//     differently.
//
// The worker has ~(kIrHeadSamples - kIrTailBlockSamples) samples of slack per
// block (~165 ms) and costs a few percent of a core. Short IRs (cab length)
// keep a single zero-latency engine and never spawn a worker.
//
// Real-time contract: the audio thread never waits for the worker. If the
// tail data for a block is not ready (the worker was descheduled), the tail is
// faded out before its ring data runs out and faded back in when the worker
// recovers (kIrTailFadeSeconds), so a late worker costs a short reverb duck
// instead of a step discontinuity. Offline renders (where the host reports
// AudioProcessor::isNonRealtime) keep a bounded wait instead, so a bounce is
// sample-exact even when driven much faster than real time. The worker is
// started as a realtime thread where the platform allows it (falling back to a
// normal thread) and polls its input ring; nothing on the audio path signals
// it, so there is no audio-thread wake-up primitive that can block.
//
// The API mirrors the subset of juce::dsp::Convolution the model loader and
// RT path use, so ChainBlock/ProcessorModelLoader only change the type.
class IrConvolver {
public:
  IrConvolver();
  ~IrConvolver();

  IrConvolver(const IrConvolver&) = delete;
  IrConvolver& operator=(const IrConvolver&) = delete;

  // Same shape as juce::dsp::Convolution::loadImpulseResponse(file, ...).
  // Reads/trims/resamples the IR to the base rate, then either installs the
  // whole kernel as one zero-latency engine (short) or splits at
  // kIrHeadSamples into a head engine (audio thread) and a tail engine
  // (worker). Must be followed by prepare() before process().
  void loadImpulseResponse(const juce::File& file,
                           juce::dsp::Convolution::Stereo stereo,
                           juce::dsp::Convolution::Trim trim,
                           size_t size,
                           juce::dsp::Convolution::Normalise normalise);

  // Rebuilds the engines for the new block size (keeping the loaded impulse)
  // and (re)starts the tail worker. Not thread-safe against process(); called
  // from the message/loader thread, never the audio thread.
  void prepare(const juce::dsp::ProcessSpec& spec);

  // Resets engine state, rings and counters. Not called on the RT path.
  void reset();

  // Audio-thread entry point. Replaces the IR block in place: feeds the tail
  // worker and adds its delayed output. Allocation-free, and never blocks in
  // real time: a tail that is not ready yet is faded out instead (see the
  // class comment). Call setNonRealtime(true) for host-driven offline renders,
  // where it may wait briefly for the worker instead of fading.
  void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept;

  // Render-mode hint from the host (AudioProcessor::isNonRealtime). Store-only,
  // safe from any thread; the value is read on the next process() call.
  void setNonRealtime(bool offline) noexcept {
    nonRealtime.store(offline, std::memory_order_relaxed);
  }

  // Advances both engines through `seconds` of silence on the calling (loader)
  // thread so JUCE's install crossfade has elapsed before the block goes live.
  // Call after prepare(), before the block is handed to the audio thread.
  void warmUp(double seconds);

  int getCurrentIRSize() const noexcept { return irSize; }
  bool isLong() const noexcept { return longIr; }

  // Diagnostics: how often the worker missed a tail block (and by how many
  // samples at worst) since prepare(). A missed block is inaudible now (the
  // tail fades instead of stepping), but a non-zero count on a running rig
  // means the worker is being descheduled. Audio-thread writes, any-thread
  // reads; relaxed is enough for counters.
  std::uint64_t getTailUnderrunCount() const noexcept {
    return tailUnderrunCount.load(std::memory_order_relaxed);
  }
  int getMaxMissedSamples() const noexcept {
    return maxMissedSamples.load(std::memory_order_relaxed);
  }
  // Test/diagnostic only: current tail gain (1 = full, 0 = faded out). Written
  // on the audio thread; safe to read after a synchronous process() call.
  float getTailGain() const noexcept { return tailGain; }

  // Test only: make the worker sleep `ms` before each batch it processes,
  // simulating a descheduled worker. Never set on a live rig.
  void setWorkerStallForTest(int ms) noexcept {
    workerStallMs.store(ms, std::memory_order_relaxed);
  }

  // Test/diagnostic only: blocks until the worker has consumed every sample
  // the audio thread has pushed, so a synchronous comparison can read the
  // tail it produced. Never called on the RT path.
  void waitForTail();

private:
  class TailWorker;  // defined in IrConvolver.cpp

  void ensureRings();
  void startWorker();
  void stopWorker();
  void resetUnderrunState();

  std::unique_ptr<juce::dsp::Convolution> headConvolver;
  std::unique_ptr<juce::dsp::Convolution> tailConvolver;
  std::unique_ptr<TailWorker> worker;
  juce::WaitableEvent workerEvent;

  juce::AudioBuffer<float> inputRing;   // numChannels x kIrTailRingSamples
  juce::AudioBuffer<float> outputRing;  // numChannels x kIrTailRingSamples

  // Sample-index counters (input-time for inputWrite/inputRead; output-time
  // for tailAvailable). All long-lived, so int64 wrap is a non-issue.
  std::atomic<int64_t> inputWrite{0};     // audio thread: samples pushed
  std::atomic<int64_t> inputRead{0};      // worker: samples consumed
  std::atomic<int64_t> tailAvailable{0};  // worker: tail ready up to this output sample
  std::atomic<bool> nonRealtime{false};   // host offline-render hint

  // Underrun diagnostics (see the getters). reset()/prepare() clear them.
  std::atomic<std::uint64_t> tailUnderrunCount{0};
  std::atomic<int> maxMissedSamples{0};
  std::atomic<int> workerStallMs{0};  // test hook only

  // Tail fade state. Audio thread only. `tailGain` rides the delayed tail
  // contribution: when the worker is late it ramps to 0 before the ring data
  // runs out and ramps back to 1 once the worker is ahead again, so a missed
  // block never splices a step into the reverb.
  float tailGain = 1.0f;
  float tailFadeStepDown = 0.0f;  // per-sample |delta| for the current fade-out
  int tailFadeRemaining = 0;      // samples left in the current fade-out
  int tailFadeSamples = 480;      // set from kIrTailFadeSeconds in prepare()
  float tailFadeStepUp = 1.0f / 480.0f;

  int hostBlock = 0;
  int numChannels = 2;  // process channels (prepared spec.numChannels, capped at 2)
  int irSize = 0;
  bool longIr = false;
};

// The head length / tail delay (170.7 ms at 48 kHz), the worker block and ring
// sizes, and the RT safety margins. Kept in the header so tests can pin the
// boundary.
inline constexpr int kIrHeadSamples = 8192;
// Small blocks keep the worker's progress (tailAvailable) fine-grained, so a
// slightly late worker loses at most this many tail samples before the fade
// takes over. The delayed tail partition is headSize-sized either way, so this
// only affects the worker's first-8192-sample engine (cheap, off-thread).
inline constexpr int kIrTailBlockSamples = 256;
inline constexpr int kIrTailRingSamples = 16384;  // power of two

// Fade applied to the tail when the worker is late (and back in when it
// recovers): at 48 kHz this is 480 samples. A late worker then costs a short
// reverb duck instead of a click. The audio thread starts the fade while at
// least this many tail samples are still in the ring, so it always reaches
// zero before the data runs out.
inline constexpr double kIrTailFadeSeconds = 0.010;

// The worker polls its ring instead of being signalled from the audio thread
// (a WaitableEvent signal takes a pthread mutex, which is not RT-safe). At
// 48 kHz this is ~240 samples, far inside the ~165 ms of slack.
inline constexpr int kIrWorkerPollMs = 5;

// Offline renders are driven faster than real time, so process() may wait this
// long for the worker before falling back to the fade. Never used in realtime
// mode. Per callback, so a dead worker degrades the bounce instead of wedging.
inline constexpr int kIrOfflineWaitMs = 50;
