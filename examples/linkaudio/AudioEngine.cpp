/* Copyright 2016, Ableton AG, Berlin. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  If you would like to incorporate Link into a proprietary software application,
 *  please contact <link-devs@ableton.com>.
 */

#include "AudioEngine.hpp"

// Make sure to define this before <cmath> is included for Windows
#define _USE_MATH_DEFINES
#include <cmath>

namespace ableton
{
namespace linkaudio
{

AudioEngine::AudioEngine(Link& link)
  : mLink(link)
  , mSampleRate(44100.)
  , mOutputLatency(0)
  , mSharedEngineData({0., false, false, 4.})
  , mLockfreeEngineData(mSharedEngineData)
  , mTimeAtLastClick{}
{
}

void AudioEngine::startPlaying()
{
  std::lock_guard<std::mutex> lock(mEngineDataGuard);
  mSharedEngineData.resetBeatTime = true;
  mSharedEngineData.isPlaying = true;
}

void AudioEngine::stopPlaying()
{
  std::lock_guard<std::mutex> lock(mEngineDataGuard);
  mSharedEngineData.isPlaying = false;
}

bool AudioEngine::isPlaying() const
{
  return mSharedEngineData.isPlaying;
}

double AudioEngine::beatTime() const
{
  const auto timeline = mLink.captureAppTimeline();
  return timeline.beatAtTime(mLink.clock().micros(), mSharedEngineData.quantum);
}

void AudioEngine::setTempo(double tempo)
{
  std::lock_guard<std::mutex> lock(mEngineDataGuard);
  mSharedEngineData.requestedTempo = tempo;
}

double AudioEngine::quantum() const
{
  return mSharedEngineData.quantum;
}

void AudioEngine::setQuantum(double quantum)
{
  std::lock_guard<std::mutex> lock(mEngineDataGuard);
  mSharedEngineData.quantum = quantum;
}

void AudioEngine::setBufferSize(std::size_t size)
{
  mBuffer = std::vector<double>(size, 0.);
}

void AudioEngine::setSampleRate(double sampleRate)
{
  mSampleRate = sampleRate;
}

AudioEngine::EngineData AudioEngine::pullEngineData()
{
  auto engineData = EngineData{};
  if (mEngineDataGuard.try_lock())
  {
    engineData.requestedTempo = mSharedEngineData.requestedTempo;
    mSharedEngineData.requestedTempo = 0;
    engineData.resetBeatTime = mSharedEngineData.resetBeatTime;
    mSharedEngineData.resetBeatTime = false;

    mLockfreeEngineData.isPlaying = mSharedEngineData.isPlaying;
    mLockfreeEngineData.quantum = mSharedEngineData.quantum;

    mEngineDataGuard.unlock();
  }
  engineData.isPlaying = mLockfreeEngineData.isPlaying;
  engineData.quantum = mLockfreeEngineData.quantum;

  return engineData;
}

void AudioEngine::renderMetronomeIntoBuffer(const Link::Timeline timeline,
  const double quantum,
  const std::chrono::microseconds beginHostTime,
  const std::size_t numSamples)
{
  using namespace std::chrono;

  // Metronome frequencies
  static const double highTone = 1567.98;
  static const double lowTone = 1108.73;
  // 100ms click duration
  static const auto clickDuration = duration<double>{0.1};

  // The number of microseconds that elapse between samples
  const auto microsPerSample = 1e6 / mSampleRate;

  for (std::size_t i = 0; i < numSamples; ++i)
  {
    double amplitude = 0.;
    // Compute the host time for this sample and the last.
    const auto hostTime = beginHostTime + microseconds(llround(i * microsPerSample));
    const auto lastSampleHostTime = hostTime - microseconds(llround(microsPerSample));

    // Only make sound for positive beat magnitudes. Negative beat
    // magnitudes are count-in beats.
    if (timeline.beatAtTime(hostTime, quantum) >= 0.)
    {
      // If the phase wraps around between the last sample and the
      // current one with respect to a 1 beat quantum, then a click
      // should occur.
      if (timeline.phaseAtTime(hostTime, 1) < timeline.phaseAtTime(lastSampleHostTime, 1))
      {
        mTimeAtLastClick = hostTime;
      }

      const auto secondsAfterClick =
        duration_cast<duration<double>>(hostTime - mTimeAtLastClick);

      // If we're within the click duration of the last beat, render
      // the click tone into this sample
      if (secondsAfterClick < clickDuration)
      {
        // If the phase of the last beat with respect to the current
        // quantum was zero, then it was at a quantum boundary and we
        // want to use the high tone. For other beats within the
        // quantum, use the low tone.
        const auto freq =
          floor(timeline.phaseAtTime(hostTime, quantum)) == 0 ? highTone : lowTone;

        // Simple cosine synth
        amplitude = cos(2 * M_PI * secondsAfterClick.count() * freq)
                    * (1 - sin(5 * M_PI * secondsAfterClick.count()));
      }
    }
    mBuffer[i] = amplitude;
  }
}

void AudioEngine::audioCallback(
  const std::chrono::microseconds hostTime, const std::size_t numSamples)
{
  const auto engineData = pullEngineData();

  auto timeline = mLink.captureAudioTimeline();

  // Clear the buffer
  std::fill(mBuffer.begin(), mBuffer.end(), 0);

  if (engineData.resetBeatTime)
  {
    // Reset the timeline so that beat 0 lands at the beginning of
    // this buffer and clear the flag.
    timeline.requestBeatAtTime(0, hostTime, engineData.quantum);
  }

  if (engineData.requestedTempo > 0)
  {
    // Set the newly requested tempo from the beginning of this buffer
    timeline.setTempo(engineData.requestedTempo, hostTime);
  }

  // Timeline modifications are complete, commit the results
  mLink.commitAudioTimeline(timeline);

  if (engineData.isPlaying)
  {
    // As long as the engine is playing, generate metronome clicks in
    // the buffer at the appropriate beats.
    renderMetronomeIntoBuffer(timeline, engineData.quantum, hostTime, numSamples);
  }
}

} // namespace linkaudio
} // namespace ableton
