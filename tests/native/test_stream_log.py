"""Exercise the actual patched StreamAPI log formatter under ASan/UBSan; no hardware."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "firmware/src/mesh/StreamAPI.cpp").read_text()
function = source.split("void StreamAPI::emitLogRecord(", 1)[1]
block = function.split("    auto num_printed =", 1)[1].split("    size_t len =", 1)[0]
block = "    auto num_printed =" + block
header = (ROOT / "firmware/src/mesh/generated/meshtastic/mesh.pb.h").read_text()
capacity = int(re.search(r"typedef struct _meshtastic_LogRecord.*?char message\[(\d+)\]", header, re.S)[1])
fixture = r'''
#include <algorithm>
#include <cassert>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
struct Formatter {
    struct { struct { char message[CAPACITY]; } log_record; } fromRadioScratchLog;
    void format(const char *format, ...) {
        memset(&fromRadioScratchLog, 0, sizeof(fromRadioScratchLog));
        va_list arg;
        va_start(arg, format);
FORMAT_BLOCK
        va_end(arg);
    }
};
int main() {
    auto *formatter = new Formatter;
    for (size_t n : {size_t(4096), size_t(0), size_t(1), size_t(CAPACITY - 2), size_t(CAPACITY - 1), size_t(CAPACITY), size_t(CAPACITY + 1)}) {
        for (bool newline : {false, true}) {
            const std::string text = std::string(n, 'a') + (newline ? "\n" : "");
            formatter->format("%s", text.c_str());
            const char *actual = formatter->fromRadioScratchLog.log_record.message;
            assert(memchr(actual, 0, CAPACITY));
            std::string expected = text.substr(0, CAPACITY - 1);
            if (!expected.empty() && expected.back() == '\n')
                expected.pop_back();
            assert(actual == expected);
        }
    }
    formatter->format("\n");
    assert(formatter->fromRadioScratchLog.log_record.message[0] == 0);
    setlocale(LC_ALL, "C");
    formatter->format("prefix %lc", static_cast<wint_t>(0xd800));
    assert(formatter->fromRadioScratchLog.log_record.message[0] == 0);
    delete formatter;
    puts("StreamAPI log boundary tests: OK");
}
'''.replace("CAPACITY", str(capacity)).replace("FORMAT_BLOCK", block)
with tempfile.TemporaryDirectory(prefix="adv-stream-log-") as directory:
    fresh = Path(directory) / "src/mesh/StreamAPI.cpp"
    fresh.parent.mkdir(parents=True)
    fresh.write_text(subprocess.check_output(
        ["git", "-C", str(ROOT / "firmware"), "show", "HEAD:src/mesh/StreamAPI.cpp"], text=True))
    subprocess.run(["git", "apply", str(ROOT / "overlay/patches/streamapi-log-format-bounds.patch")],
                   cwd=directory, check=True)
    shipped_block = fresh.read_text().split("void StreamAPI::emitLogRecord(", 1)[1].split(
        "    auto num_printed =", 1)[1].split("    size_t len =", 1)[0]
    assert block == "    auto num_printed =" + shipped_block, "actual formatter differs from shipped patch"
    binary = Path(directory) / "test-stream-log"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-x", "c++", "-",
                    "-o", str(binary)], input=fixture, text=True, check=True)
    subprocess.run([str(binary)], check=True)
