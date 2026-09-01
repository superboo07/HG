#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PSPadLiveState
{
    uint16_t buttons = 0;
    uint8_t leftX = 0x80;
    uint8_t leftY = 0x80;
    uint8_t rightX = 0x80;
    uint8_t rightY = 0x80;
};

// Parses the diagnostic live-input form state:buttons:lx:ly:rx:ry.
// Button bits are active-high and stick values are the raw PS2 0..255 bytes.
bool parsePSPadLiveStateCommand(std::string_view command, PSPadLiveState &state);

class PSPadReplay
{
public:
    bool configure(std::string_view specification, std::string *error = nullptr);
    void reset();
    PSPadLiveState nextState();
    uint16_t nextActiveHighButtons();
    bool configured() const { return m_configured; }
    uint64_t readIndex() const { return m_readIndex; }

private:
    struct Event
    {
        uint64_t firstRead = 0;
        uint64_t endRead = 0;
        PSPadLiveState state{};
    };

    std::vector<Event> m_events;
    uint64_t m_readIndex = 0;
    size_t m_nextEvent = 0;
    bool m_configured = false;
};

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

private:
    void initializeReplay();

    PSPadReplay m_replay;
    bool m_replayInitialized = false;
    uint16_t m_lastReplayButtons = 0;
};

#endif
