#include "AdvNodeCount.h"
#include "AdvOrder.h"
#include "AdvStorage.h"
#include "AdvUtf8.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>

using namespace advui;

int main()
{
    assert(!dirtyWriteDue(2999, 0, 3000));
    assert(dirtyWriteDue(3000, 0, 3000));
    assert(!dirtyWriteDue(30000, 29999, 3000)); // a busy stream restarts the quiet window
    assert(dirtyWriteDue(10, 0xFFFFFF00U, 200)); // millis rollover

    // Every config-stream entry contributes to the authoritative DB count.
    assert(shouldIncrementNodesSeen(false, -1, 64, 64));
    assert(!shouldIncrementNodesSeen(false, 12, 64, 64)); // duplicate delivery
    // During the live phase an absent node is provably new only while there is
    // still room in the mirror. A full mirror may have evicted this identity.
    assert(shouldIncrementNodesSeen(true, -1, 63, 64));
    assert(!shouldIncrementNodesSeen(true, -1, 64, 64));
    assert(!shouldIncrementNodesSeen(true, 12, 63, 64));

    Utf8Rune ascii = utf8Decode("A");
    assert(ascii.valid && ascii.bytes == 1 && ascii.codepoint == 'A');
    Utf8Rune cyr = utf8Decode("Ж");
    assert(cyr.valid && cyr.bytes == 2 && cyr.codepoint == 0x416);
    Utf8Rune emoji = utf8Decode("💀");
    assert(emoji.valid && emoji.bytes == 4 && emoji.codepoint == 0x1F480);

    const char truncated[] = {(char)0xE2, (char)0x82, 0};
    assert(!utf8Decode(truncated).valid && utf8Decode(truncated).bytes == 1);
    const char overlong[] = {(char)0xC0, (char)0xAF, 0};
    assert(!utf8Decode(overlong).valid);
    const char surrogate[] = {(char)0xED, (char)0xA0, (char)0x80, 0};
    assert(!utf8Decode(surrogate).valid);
    const char tooHigh[] = {(char)0xF4, (char)0x90, (char)0x80, (char)0x80, 0};
    assert(!utf8Decode(tooHigh).valid);

    char copy[4];
    assert(utf8CopyValid(copy, sizeof(copy), "AЖB") == 3);
    assert(strcmp(copy, "AЖ") == 0);

    char ownerName[40];
    assert(utf8CopyValid(ownerName, sizeof(ownerName), "яяяяяяяяяяяяяяяяяяяя") == 38);
    assert(utf8Decode(ownerName + 36).valid);
    assert(ownerName[38] == 0);

    char channelName[12];
    assert(utf8CopyValid(channelName, sizeof(channelName), "яяяяяя") == 10);
    assert(utf8Decode(channelName + 8).valid);
    assert(channelName[10] == 0);

    char invalid[] = {'x', (char)0x80, 'y', 0};
    utf8Sanitize(invalid, sizeof(invalid));
    assert(strcmp(invalid, "x?y") == 0);

    char trim[] = "AЖ💀";
    assert(utf8TrimLast(trim) && strcmp(trim, "AЖ") == 0);
    assert(utf8TrimLast(trim) && strcmp(trim, "A") == 0);
    assert(utf8TrimLast(trim) && trim[0] == 0);
    assert(!utf8TrimLast(trim));

    size_t total = 0;
    assert(checkedRecordRegion(32, 260, 4, 16, 8384, &total));
    assert(total == 8384);
    assert(!checkedRecordRegion(32, 260, 4, 16, 8383, nullptr));
    assert(!checkedRecordRegion(32, 260, 4, 16, 8385, nullptr));
    assert(checkedRecordRegion(32, 260, 4, 16, 8385, nullptr, true));
    assert(!checkedRecordRegion(std::numeric_limits<size_t>::max(), 2, 1, 1,
                                std::numeric_limits<size_t>::max(), nullptr));

    size_t records = 0;
    assert(checkedFixedRecordFile(4, 4, 260, &records) && records == 0);
    assert(checkedFixedRecordFile(4 + 3 * 260, 4, 260, &records) && records == 3);
    assert(!checkedFixedRecordFile(3, 4, 260, nullptr));
    assert(!checkedFixedRecordFile(4 + 260 + 1, 4, 260, nullptr));
    assert(!checkedFixedRecordFile(4, 4, 0, nullptr));

    assert(validUtcOffsetMinutes(-12 * 60));
    assert(validUtcOffsetMinutes(14 * 60));
    assert(!validUtcOffsetMinutes(-12 * 60 - 1));
    assert(!validUtcOffsetMinutes(14 * 60 + 1));
    assert(!validUtcOffsetMinutes(std::numeric_limits<int32_t>::min()));
    assert(!validUtcOffsetMinutes(std::numeric_limits<int32_t>::max()));

    assert(!deadlineReached(100, 0));
    assert(!deadlineReached(0xfffffff0U, 10));
    assert(deadlineReached(10, 10));
    assert(deadlineReached(11, 10));
    assert(deadlineReached(20, 0xfffffff0U));
    assert(armDeadline(100, 400) == 500);
    assert(armDeadline(0xffffffffU - 399U, 400) == 1);

    assert(validBleAddress("0a:1b:2c:3d:4e:5f"));
    assert(validBleAddress("0A:1B:2C:3D:4E:5F"));
    assert(!validBleAddress(nullptr));
    assert(!validBleAddress(""));
    assert(!validBleAddress("02:11:22:33:44"));
    assert(!validBleAddress("02-11-22-33-44-55"));
    assert(!validBleAddress("02:11:22:33:44:5g"));
    assert(!validBleAddress("02:11:22:33:44:55:66"));
    uint8_t bleAddress[6] = {};
    assert(parseBleAddress("02:11:aB:33:44:fF", bleAddress));
    const uint8_t expectedBleAddress[] = {0x02, 0x11, 0xab, 0x33, 0x44, 0xff};
    assert(memcmp(bleAddress, expectedBleAddress, sizeof(bleAddress)) == 0);
    memset(bleAddress, 0xa5, sizeof(bleAddress));
    assert(!parseBleAddress("02:11:22:33:44:5g", bleAddress));
    const uint8_t untouchedBleAddress[] = {0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
    assert(memcmp(bleAddress, untouchedBleAddress, sizeof(bleAddress)) == 0);

    uint8_t channels[] = {0, 1, 2, 3, 4, 5};
    stableMoveMatchingFirst(channels, sizeof(channels), [](uint8_t channel) { return channel == 1 || channel == 4; });
    const uint8_t expectedChannels[] = {1, 4, 0, 2, 3, 5};
    assert(memcmp(channels, expectedChannels, sizeof(channels)) == 0);
    stableMoveMatchingFirst<uint8_t>(nullptr, 0, [](uint8_t) { return true; });

    float frequency = -1.0f;
    assert(parseFrequencyOverride("", &frequency) && frequency == 0.0f);
    assert(parseFrequencyOverride("433.525", &frequency) && frequency > 433.52f && frequency < 433.53f);
    assert(parseFrequencyOverride("0", &frequency) && frequency == 0.0f);
    assert(parseFrequencyOverride("150", &frequency) && frequency == 150.0f);
    assert(parseFrequencyOverride("960", &frequency) && frequency == 960.0f);
    assert(!parseFrequencyOverride("149.999", &frequency));
    assert(!parseFrequencyOverride("960.001", &frequency));
    assert(!parseFrequencyOverride("999999999999999999999999999999999999999999", &frequency));
    assert(!parseFrequencyOverride(".", &frequency));
    assert(!parseFrequencyOverride("433..525", &frequency));
    assert(!parseFrequencyOverride("433.5x", &frequency));
    assert(!parseFrequencyOverride(nullptr, &frequency));

    int hour = -1, minute = -1;
    assert(parseClockHm("00:10", &hour, &minute) && hour == 0 && minute == 10);
    assert(parseClockHm("9:05", &hour, &minute) && hour == 9 && minute == 5);
    assert(parseClockHm("2359", &hour, &minute) && hour == 23 && minute == 59);
    assert(!parseClockHm("24:00", &hour, &minute));
    assert(!parseClockHm("12:60", &hour, &minute));
    assert(!parseClockHm("12:34:", &hour, &minute));
    assert(!parseClockHm("12::34", &hour, &minute));
    assert(!parseClockHm(nullptr, &hour, &minute));

    assert(!validStoredEpoch(1599999999U));
    assert(validStoredEpoch(1600000000U));
    assert(validStoredEpoch(4102444800U));
    assert(!validStoredEpoch(4102444801U));
    assert(!validStoredEpoch(0xffffffffU));

    OldestSlot oldest[3];
    size_t oldestCount = 0;
    considerOldestSlot(oldest, 3, &oldestCount, 40, 400);
    considerOldestSlot(oldest, 3, &oldestCount, 20, 200);
    considerOldestSlot(oldest, 3, &oldestCount, 30, 300);
    considerOldestSlot(oldest, 3, &oldestCount, 10, 100); // replaces 400
    considerOldestSlot(oldest, 3, &oldestCount, 50, 500); // too new: ignored
    assert(oldestCount == 3);
    bool have100 = false, have200 = false, have300 = false;
    for (const auto &slot : oldest) {
        have100 = have100 || (slot.position == 10 && slot.stamp == 100);
        have200 = have200 || (slot.position == 20 && slot.stamp == 200);
        have300 = have300 || (slot.position == 30 && slot.stamp == 300);
    }
    assert(have100 && have200 && have300);
    considerOldestSlot(nullptr, 3, &oldestCount, 0, 0);
    considerOldestSlot(oldest, 0, &oldestCount, 0, 0);
    return 0;
}
