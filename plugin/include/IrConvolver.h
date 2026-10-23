#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
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
// block (~150 ms) and costs a few percent of a core. Short IRs (cab length)
// keep a single zero-latency engine and never spawn a worker.
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
  // worker and adds its delayed output. Allocation-free.
  void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept;

  // Advances both engines through `seconds` of silence on the calling (loader)
  // thread so JUCE's install crossfade has elapsed before the block goes live.
  // Call after prepare(), before the block is handed to the audio thread.
  void warmUp(double seconds);

  int getCurrentIRSize() const noexcept { return irSize; }
  bool isLong() const noexcept { return longIr; }

  // Test/diagnostic only: blocks until the worker has consumed every sample
  // the audio thread has pushed, so a synchronous comparison can read the
  // tail it produced. Never called on the RT path.
  void waitForTail();

private:
  class TailWorker;  // defined in IrConvolver.cpp

  void ensureRings();
  void startWorker();
  void stopWorker();

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

  int hostBlock = 0;
  int numChannels = 2;  // process channels (prepared spec.numChannels, capped at 2)
  int irSize = 0;
  bool longIr = false;
};

// The head length / tail delay (170.7 ms at 48 kHz), and the worker block and
// ring sizes. Kept in the header so tests can pin the boundary.
inline constexpr int kIrHeadSamples = 8192;
inline constexpr int kIrTailBlockSamples = 1024;
inline constexpr int kIrTailRingSamples = 16384;  // power of two
