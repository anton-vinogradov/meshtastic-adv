import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "overlay/patches/streamapi-retained-nonblocking-writes.patch"


def added_file(patch: str, relative_path: str) -> str:
    marker = f"diff --git a/{relative_path} b/{relative_path}\n"
    section = patch.split(marker, 1)[1].split("\ndiff --git ", 1)[0]
    body = section.split("@@ -0,0", 1)[1].split("\n", 1)[1]
    return "\n".join(line[1:] for line in body.splitlines() if line.startswith("+") and not line.startswith("+++")) + "\n"


class StreamFrameWriterBehaviorTests(unittest.TestCase):
    def test_partial_usb_writes_are_retained_without_loss_or_duplication(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")

        patch = PATCH.read_text()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "StreamFrameWriter.h").write_text(added_file(patch, "src/mesh/StreamFrameWriter.h"))
            (root / "StreamFrameWriter.cpp").write_text(added_file(patch, "src/mesh/StreamFrameWriter.cpp"))
            (root / "UsbSessionStream.h").write_text(added_file(patch, "src/mesh/UsbSessionStream.h"))
            (root / "Stream.h").write_text(
                """#pragma once
#include <cstddef>
#include <cstdint>
class Stream {
public:
    virtual ~Stream() = default;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *, size_t) = 0;
    virtual int availableForWrite() = 0;
};
"""
            )
            (root / "HWCDC.h").write_text(
                """#pragma once
class HWCDC {
public:
    inline static bool plugged = true;
    inline static bool connected = true;
    static bool isPlugged() { return plugged; }
    static bool isConnected() { return connected; }
};
"""
            )
            (root / "main.cpp").write_text(
                """#include "StreamFrameWriter.h"
#include "UsbSessionStream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

class ScriptedStream : public Stream {
public:
    int capacity = 0;
    bool disconnectAfterWrite = false;
    int writes = 0;
    std::vector<uint8_t> output;

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *buffer, size_t size) override {
        ++writes;
        const size_t sent = std::min(size, static_cast<size_t>(capacity));
        output.insert(output.end(), buffer, buffer + sent);
        if (disconnectAfterWrite) {
            HWCDC::connected = false;
            disconnectAfterWrite = false;
        }
        return sent;
    }
    int availableForWrite() override { return capacity; }
};

int main() {
    ScriptedStream raw;
    UsbSessionStream usb(raw);
    StreamFrameWriter writer;
    uint8_t frame[] = {1, 2, 3, 4, 5, 6};

    // The first call can lose its CDC-connected state internally, but the
    // adapter gives stock HWCDC only the two bytes already known to fit.
    raw.capacity = 2;
    raw.disconnectAfterWrite = true;
    assert(!writer.writeFrame(usb, frame, sizeof(frame), false));
    assert((raw.output == std::vector<uint8_t>{1, 2}));

    // No call reaches the delegate while its internal CDC flag is false.
    const int writesBefore = raw.writes;
    assert(!writer.finishPendingFrame(usb));
    assert(raw.writes == writesBefore);

    // Recovery resumes at the exact retained offset: no dropped prefix and no
    // duplicated bytes.
    HWCDC::connected = true;
    raw.capacity = 4;
    assert(writer.finishPendingFrame(usb));
    assert(writer.isIdle());
    assert((raw.output == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));

    // Best-effort logs never retain or partially start without full capacity.
    uint8_t logFrame[] = {7, 8, 9};
    raw.capacity = 2;
    assert(!writer.writeFrame(usb, logFrame, sizeof(logFrame), true));
    assert(writer.isIdle());
    assert(raw.output.size() == sizeof(frame));

    // A physical disconnect also prevents the delegate's drop-on-disconnect path.
    HWCDC::plugged = false;
    raw.capacity = 64;
    assert(!writer.writeFrame(usb, logFrame, sizeof(logFrame), true));
    assert(raw.output.size() == sizeof(frame));
    return 0;
}
"""
            )
            binary = root / "stream-writer-test"
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                 str(root / "StreamFrameWriter.cpp"), str(root / "main.cpp"), "-o", str(binary)],
                text=True,
                capture_output=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
