#include "MidiOutputSender.h"

MidiOutputSender::MidiOutputSender() : juce::Thread ("Bass2MIDI MIDI sender") {}

MidiOutputSender::~MidiOutputSender()
{
    close();
}

bool MidiOutputSender::openVirtualPort (const juce::String& portName)
{
    close();

    output = juce::MidiOutput::createNewDevice (portName);
    if (output == nullptr)
        return false;

    startThread (juce::Thread::Priority::highest);
    return true;
}

void MidiOutputSender::close()
{
    stopThread (1000);
    output.reset();
}

juce::String MidiOutputSender::getPortName() const
{
    return output != nullptr ? output->getName() : juce::String();
}

MidiOutputSender::ShortMessage MidiOutputSender::noteOn (int channel1to16, int note, int velocity) noexcept
{
    ShortMessage m;
    m.bytes = { static_cast<std::uint8_t> (0x90 | ((channel1to16 - 1) & 0x0f)),
                static_cast<std::uint8_t> (note & 0x7f),
                static_cast<std::uint8_t> (juce::jlimit (1, 127, velocity)) };
    m.size = 3;
    return m;
}

MidiOutputSender::ShortMessage MidiOutputSender::noteOff (int channel1to16, int note) noexcept
{
    ShortMessage m;
    m.bytes = { static_cast<std::uint8_t> (0x80 | ((channel1to16 - 1) & 0x0f)),
                static_cast<std::uint8_t> (note & 0x7f),
                0 };
    m.size = 3;
    return m;
}

bool MidiOutputSender::Fifo::push (const ShortMessage& message, std::atomic<std::uint32_t>& drops) noexcept
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 + scope.blockSize2 != 1)
    {
        drops.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    slots[static_cast<size_t> (scope.blockSize1 == 1 ? scope.startIndex1 : scope.startIndex2)] = message;
    return true;
}

template <typename Fn>
void MidiOutputSender::Fifo::drain (Fn&& fn) noexcept
{
    const auto scope = fifo.read (fifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i)
        fn (slots[static_cast<size_t> (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        fn (slots[static_cast<size_t> (scope.startIndex2 + i)]);
}

void MidiOutputSender::run()
{
    const auto send = [this] (const ShortMessage& m)
    {
        output->sendMessageNow (juce::MidiMessage (m.bytes.data(), m.size));
    };

    while (! threadShouldExit())
    {
        audioFifo.drain (send);
        messageFifo.drain (send);
        wait (senderPollIntervalMs);
    }

    // Deliver anything queued before shutdown so Note Offs are not lost.
    audioFifo.drain (send);
    messageFifo.drain (send);
}
