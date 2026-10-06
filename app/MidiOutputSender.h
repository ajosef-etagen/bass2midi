#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// Owns the virtual MIDI output port ("Bass2MIDI" in MainStage's MIDI input list) and the thread
// that writes to it.
//
// Cross-thread boundary: producers never touch the MIDI device. They push 1-3 byte channel
// messages into lock-free single-producer/single-consumer FIFOs (one for the audio thread, one for
// the message thread). A dedicated sender thread drains both and calls MidiOutput::sendMessageNow.
//
// Latency added: up to senderPollIntervalMs between a push and the send (the sender thread
// polls; signalling it from the audio thread would need a lock).
class MidiOutputSender : private juce::Thread
{
public:
    struct ShortMessage
    {
        std::array<std::uint8_t, 3> bytes {};
        std::uint8_t size = 0;
    };

    static constexpr int senderPollIntervalMs = 1;
    static constexpr int fifoCapacity = 256;

    MidiOutputSender();
    ~MidiOutputSender() override;

    // Message thread. Creates the virtual port (macOS/Linux; returns false where the OS has no
    // virtual MIDI ports, e.g. Windows) and starts the sender thread.
    bool openVirtualPort (const juce::String& portName);
    void close();
    bool isOpen() const noexcept { return output != nullptr; }
    juce::String getPortName() const;

    // Audio thread only (single producer). Real-time safe; returns false and counts a drop when full.
    bool pushFromAudioThread (const ShortMessage& message) noexcept { return audioFifo.push (message, droppedMessages); }

    // Message thread only (single producer).
    bool pushFromMessageThread (const ShortMessage& message) noexcept { return messageFifo.push (message, droppedMessages); }

    std::uint32_t getDroppedMessageCount() const noexcept { return droppedMessages.load(); }

    static ShortMessage noteOn (int channel1to16, int note, int velocity) noexcept;
    static ShortMessage noteOff (int channel1to16, int note) noexcept;

private:
    struct Fifo
    {
        juce::AbstractFifo fifo { fifoCapacity };
        std::array<ShortMessage, fifoCapacity> slots {};

        bool push (const ShortMessage& message, std::atomic<std::uint32_t>& drops) noexcept;
        template <typename Fn> void drain (Fn&& fn) noexcept;
    };

    void run() override;

    Fifo audioFifo, messageFifo;
    std::atomic<std::uint32_t> droppedMessages { 0 };
    std::unique_ptr<juce::MidiOutput> output; // created/destroyed only while the thread is stopped
};
