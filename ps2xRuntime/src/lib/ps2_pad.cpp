#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    bool parseUnsigned(std::string_view text, uint64_t maximum, uint64_t &value)
    {
        if (text.empty() || text.front() == '-')
            return false;

        const std::string owned(text);
        char *end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 0);
        if (errno == ERANGE || end == owned.c_str() || *end != '\0' || parsed > maximum)
            return false;

        value = static_cast<uint64_t>(parsed);
        return true;
    }
}

bool parsePSPadLiveStateCommand(std::string_view command, PSPadLiveState &state)
{
    constexpr std::string_view prefix = "state:";
    if (!command.starts_with(prefix))
        return false;

    std::string_view payload = command.substr(prefix.size());
    uint64_t values[5] = {};
    constexpr uint64_t maxima[5] = {
        std::numeric_limits<uint16_t>::max(), 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    for (size_t index = 0; index < 5; ++index)
    {
        const size_t separator = payload.find(':');
        if ((index < 4 && separator == std::string_view::npos) ||
            (index == 4 && separator != std::string_view::npos))
            return false;

        const std::string_view field = index < 4 ? payload.substr(0, separator) : payload;
        if (!parseUnsigned(field, maxima[index], values[index]))
            return false;
        if (index < 4)
            payload.remove_prefix(separator + 1);
    }

    state.buttons = static_cast<uint16_t>(values[0]);
    state.leftX = static_cast<uint8_t>(values[1]);
    state.leftY = static_cast<uint8_t>(values[2]);
    state.rightX = static_cast<uint8_t>(values[3]);
    state.rightY = static_cast<uint8_t>(values[4]);
    return true;
}

bool PSPadReplay::configure(std::string_view specification, std::string *error)
{
    reset();
    m_events.clear();
    m_configured = false;

    auto fail = [&](const std::string &message)
    {
        m_events.clear();
        if (error)
            *error = message;
        return false;
    };

    if (specification.empty())
        return fail("replay specification is empty");

    size_t cursor = 0;
    while (cursor < specification.size())
    {
        const size_t comma = specification.find(',', cursor);
        const std::string_view entry = specification.substr(
            cursor, comma == std::string_view::npos ? specification.size() - cursor : comma - cursor);
        const size_t dash = entry.find('-');
        const size_t colon = entry.find(':');
        if (dash == std::string_view::npos || colon == std::string_view::npos || dash >= colon ||
            entry.find('-', dash + 1) != std::string_view::npos)
            return fail("expected comma-separated first-end:buttons[:lx:ly:rx:ry] entries");

        uint64_t first = 0;
        uint64_t end = 0;
        uint64_t values[5] = {0u, kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter};
        if (!parseUnsigned(entry.substr(0, dash), std::numeric_limits<uint64_t>::max(), first) ||
            !parseUnsigned(entry.substr(dash + 1, colon - dash - 1), std::numeric_limits<uint64_t>::max(), end))
            return fail("replay entry contains an invalid number");

        std::string_view fields = entry.substr(colon + 1u);
        size_t fieldCount = 0u;
        while (!fields.empty() && fieldCount < 5u)
        {
            const size_t separator = fields.find(':');
            const std::string_view field = separator == std::string_view::npos
                                               ? fields
                                               : fields.substr(0u, separator);
            const uint64_t maximum = fieldCount == 0u
                                         ? std::numeric_limits<uint16_t>::max()
                                         : std::numeric_limits<uint8_t>::max();
            if (!parseUnsigned(field, maximum, values[fieldCount]))
                return fail("replay entry contains an invalid state field");
            ++fieldCount;
            if (separator == std::string_view::npos)
            {
                fields = {};
                break;
            }
            fields.remove_prefix(separator + 1u);
        }
        if (!fields.empty() || (fieldCount != 1u && fieldCount != 5u))
            return fail("replay state must contain buttons alone or buttons plus four axes");
        if (first >= end)
            return fail("replay event end must be greater than its first read");
        if (!m_events.empty() && first < m_events.back().endRead)
            return fail("replay events must be sorted and non-overlapping");

        PSPadLiveState state{};
        state.buttons = static_cast<uint16_t>(values[0]);
        state.leftX = static_cast<uint8_t>(values[1]);
        state.leftY = static_cast<uint8_t>(values[2]);
        state.rightX = static_cast<uint8_t>(values[3]);
        state.rightY = static_cast<uint8_t>(values[4]);
        m_events.push_back(Event{first, end, state});
        if (comma == std::string_view::npos)
            break;
        cursor = comma + 1;
        if (cursor == specification.size())
            return fail("replay specification has a trailing comma");
    }

    m_configured = true;
    if (error)
        error->clear();
    return true;
}

void PSPadReplay::reset()
{
    m_readIndex = 0;
    m_nextEvent = 0;
}

PSPadLiveState PSPadReplay::nextState()
{
    if (!m_configured)
        return {};

    while (m_nextEvent < m_events.size() && m_readIndex >= m_events[m_nextEvent].endRead)
        ++m_nextEvent;

    PSPadLiveState state{};
    if (m_nextEvent < m_events.size())
    {
        const Event &event = m_events[m_nextEvent];
        if (m_readIndex >= event.firstRead)
            state = event.state;
    }
    ++m_readIndex;
    return state;
}

uint16_t PSPadReplay::nextActiveHighButtons()
{
    return nextState().buttons;
}

void PSPadBackend::initializeReplay()
{
    m_replayInitialized = true;
    const char *specification = std::getenv("PS2X_PAD_REPLAY");
    if (!specification || specification[0] == '\0')
        return;

    std::string error;
    if (!m_replay.configure(specification, &error))
    {
        std::cerr << "[pad] invalid PS2X_PAD_REPLAY: " << error << std::endl;
        return;
    }
    std::cerr << "[pad] deterministic replay enabled: " << specification << std::endl;
}

bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto applyKeyboardInput = [&]()
    {
        auto clearBit = [&btns](uint16_t mask)
        { btns &= ~mask; };

        if (IsKeyDown(KEY_UP))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);

        // Keep the arrow keys as the digital D-pad and use WASD as a full
        // left-stick replacement so keyboard users can steer games that read
        // only the analog axes.
        if (IsKeyDown(KEY_A) != IsKeyDown(KEY_D))
            data[6] = IsKeyDown(KEY_A) ? 0u : 255u;
        if (IsKeyDown(KEY_W) != IsKeyDown(KEY_S))
            data[7] = IsKeyDown(KEY_W) ? 0u : 255u;
    };

    if (!m_replayInitialized)
        initializeReplay();
    const char *liveFilePath = std::getenv("PS2X_PAD_LIVE_FILE");
    const bool liveFileConfigured = liveFilePath && liveFilePath[0] != '\0';
    if ((m_replay.configured() || liveFileConfigured) && port == 0 && slot == 0)
    {
        static uint64_t liveOnlyReadIndex = 0;
        const PSPadLiveState replayState = m_replay.configured() ? m_replay.nextState() : PSPadLiveState{};
        const uint16_t replayButtons = replayState.buttons;
        const uint64_t replayRead = m_replay.configured() ? m_replay.readIndex() - 1u : liveOnlyReadIndex++;
        if (m_replay.configured())
        {
            data[6] = replayState.leftX;
            data[7] = replayState.leftY;
            data[4] = replayState.rightX;
            data[5] = replayState.rightY;
        }
        static const uint64_t replayTraceInterval = []
        {
            const char *text = std::getenv("PS2X_PAD_REPLAY_TRACE_INTERVAL");
            if (!text || text[0] == '\0')
                return uint64_t{0};
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(text, &end, 10);
            return (end != text && *end == '\0') ? static_cast<uint64_t>(parsed) : uint64_t{0};
        }();
        const bool replayButtonsChanged = replayButtons != m_lastReplayButtons;
        const bool replayPeriodicTrace = replayTraceInterval != 0u &&
                                         (replayRead % replayTraceInterval) == 0u;
        if (m_replay.configured() && (replayButtonsChanged || replayPeriodicTrace))
        {
            std::cerr << "[pad] replay read=" << replayRead
                      << " buttons=0x" << std::hex << replayButtons << std::dec << std::endl;
            if (replayButtonsChanged)
                m_lastReplayButtons = replayButtons;
        }

        uint16_t liveButtons = 0;
        if (liveFileConfigured)
        {
            std::ifstream liveFile(liveFilePath);
            std::string valueText;
            uint64_t parsedValue = 0;
            if (liveFile >> valueText)
            {
                static std::string lastLiveCommand;
                static uint64_t skipSequenceStart = std::numeric_limits<uint64_t>::max();
                static uint64_t pulseSequenceStart = std::numeric_limits<uint64_t>::max();
                static uint16_t pulseSequenceButtons = 0;
                constexpr std::string_view skipPrefix = "skip:";
                constexpr std::string_view pulsePrefix = "pulse:";
                PSPadLiveState liveState;
                if (parsePSPadLiveStateCommand(valueText, liveState))
                {
                    liveButtons = liveState.buttons;
                    data[6] = liveState.leftX;
                    data[7] = liveState.leftY;
                    data[4] = liveState.rightX;
                    data[5] = liveState.rightY;
                    if (valueText != lastLiveCommand)
                    {
                        lastLiveCommand = valueText;
                        std::cerr << "[pad] live-file state read=" << replayRead
                                  << " buttons=0x" << std::hex << liveButtons << std::dec
                                  << " lx=" << static_cast<unsigned>(liveState.leftX)
                                  << " ly=" << static_cast<unsigned>(liveState.leftY)
                                  << " rx=" << static_cast<unsigned>(liveState.rightX)
                                  << " ry=" << static_cast<unsigned>(liveState.rightY) << std::endl;
                    }
                }
                else if (valueText.starts_with(pulsePrefix))
                {
                    const std::string_view payload = std::string_view(valueText).substr(pulsePrefix.size());
                    const size_t separator = payload.find(':');
                    uint64_t generation = 0;
                    if (separator != std::string_view::npos &&
                        parseUnsigned(payload.substr(0, separator), std::numeric_limits<uint16_t>::max(), parsedValue) &&
                        parseUnsigned(payload.substr(separator + 1), std::numeric_limits<uint64_t>::max(), generation))
                    {
                        if (valueText != lastLiveCommand)
                        {
                            lastLiveCommand = valueText;
                            pulseSequenceStart = replayRead;
                            pulseSequenceButtons = static_cast<uint16_t>(parsedValue);
                            std::cerr << "[pad] live-file pulse-sequence read=" << replayRead
                                      << " buttons=0x" << std::hex << pulseSequenceButtons << std::dec
                                      << " generation=" << generation << std::endl;
                        }

                        if (pulseSequenceStart != std::numeric_limits<uint64_t>::max())
                        {
                            const uint64_t offset = replayRead - pulseSequenceStart;
                            if (offset < 3u)
                                liveButtons = pulseSequenceButtons;
                            else
                                pulseSequenceStart = std::numeric_limits<uint64_t>::max();
                        }
                    }
                }
                else if (valueText.starts_with(skipPrefix) &&
                    parseUnsigned(std::string_view(valueText).substr(skipPrefix.size()),
                                  std::numeric_limits<uint64_t>::max(), parsedValue))
                {
                    if (valueText != lastLiveCommand)
                    {
                        lastLiveCommand = valueText;
                        skipSequenceStart = replayRead;
                        std::cerr << "[pad] live-file skip-sequence read=" << replayRead
                                  << " generation=" << parsedValue << std::endl;
                    }

                    if (skipSequenceStart != std::numeric_limits<uint64_t>::max())
                    {
                        const uint64_t offset = replayRead - skipSequenceStart;
                        if (offset < 3u)
                            liveButtons = PAD_START;
                        else if (offset >= 8u && offset < 11u)
                            liveButtons = PAD_TRIANGLE;
                        else if (offset >= 11u)
                            skipSequenceStart = std::numeric_limits<uint64_t>::max();
                    }
                }
                else if (parseUnsigned(valueText, std::numeric_limits<uint16_t>::max(), parsedValue))
                {
                    liveButtons = static_cast<uint16_t>(parsedValue);
                }
            }

            static uint16_t lastLiveButtons = std::numeric_limits<uint16_t>::max();
            if (liveButtons != lastLiveButtons)
            {
                std::cerr << "[pad] live-file read=" << replayRead
                          << " buttons=0x" << std::hex << liveButtons << std::dec << std::endl;
                lastLiveButtons = liveButtons;
            }
        }
        static const uint64_t replayTracePeriod = []()
        {
            const char *value = std::getenv("PS2X_PAD_REPLAY_TRACE_PERIOD");
            return value && value[0] != '\0' ? std::strtoull(value, nullptr, 0) : 0u;
        }();
        if (replayTracePeriod != 0u && (replayRead % replayTracePeriod) == 0u)
        {
            std::cerr << "[pad] replay checkpoint read=" << replayRead << std::endl;
        }
        btns = static_cast<uint16_t>(btns & ~(replayButtons | liveButtons));
        if (std::getenv("PS2X_PAD_ALLOW_KEYBOARD_WITH_REPLAY") != nullptr)
            applyKeyboardInput();
        data[2] = static_cast<uint8_t>(btns & 0xFF);
        data[3] = static_cast<uint8_t>(btns >> 8);
        return true;
    }

    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    applyKeyboardInput();

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
