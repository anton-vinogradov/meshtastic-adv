"""Exercise the shipped RadioLib singleton lifetime with ASan/UBSan, no hardware."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--upstream-repro", action="store_true", help="require the pinned upstream use-after-free")
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="adv-radio-lifetime-") as directory:
    directory = Path(directory)
    source_path = directory / "src/mesh/RadioLibInterface.cpp"
    header_path = directory / "src/mesh/RadioLibInterface.h"
    source_path.parent.mkdir(parents=True)
    for path in (source_path, header_path):
        path.write_bytes(subprocess.check_output([
            "git", "-C", str(ROOT / "firmware"), "show", "HEAD:" + str(path.relative_to(directory))]))
    if not args.upstream_repro:
        subprocess.run(["git", "apply", str(ROOT / "overlay/patches/radiolib-instance-lifetime.patch")],
                       cwd=directory, check=True)
    source = source_path.read_text()
    signatures = ["RadioLibInterface::RadioLibInterface(", "void RadioLibInterface::pollMissedIrqs()"]
    destructor = ""
    declaration = ""
    if not args.upstream_repro:
        signatures.append("RadioLibInterface::~RadioLibInterface()")
        declaration = "~RadioLibInterface() override;"
        assert declaration in header_path.read_text()
        actual = (ROOT / "firmware/src/mesh/RadioLibInterface.cpp").read_text()
        for signature in signatures:
            assert function(actual, signature) == function(source, signature), \
                "synchronized radio lifetime differs from shipped patch"
        destructor = function(source, signatures[-1])

    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
using RADIOLIB_PIN_TYPE = int;
constexpr uint8_t NOISE_FLOOR_SAMPLES = 8;
constexpr int NOISE_FLOOR_DEFAULT = -120;
struct LockingArduinoHal {};
struct PhysicalLayer {};
struct NotifiedWorkerThread {
    explicit NotifiedWorkerThread(const char *) {}
    virtual ~NotifiedWorkerThread() = default;
};
struct Module {
    Module(LockingArduinoHal *, int, int, int, int) {}
};
struct RadioLibInterface : NotifiedWorkerThread {
    static RadioLibInterface *instance;
    Module module;
    PhysicalLayer *iface;
    int noiseFloorSamples[NOISE_FLOOR_SAMPLES];
    bool isReceiving = false;
    RadioLibInterface(LockingArduinoHal *, int, int, int, int, PhysicalLayer *);
    DESTRUCTOR_DECLARATION
    void checkRxDoneIrqFlag() { assert(iface); }
    void pollMissedIrqs();
};
RadioLibInterface *RadioLibInterface::instance;
CONSTRUCTOR
DESTRUCTOR
POLL
static auto probe() { return std::make_unique<RadioLibInterface>(nullptr, 5, 4, 3, 6, nullptr); }
static void mainLoopPoll() {
    if (RadioLibInterface::instance != nullptr)
        RadioLibInterface::instance->pollMissedIrqs();
}
int main() {
    // Both TCXO and XTAL probes fail when Cap LoRa is physically absent.
    for (int cycle = 0; cycle != 1000; ++cycle) {
        for (int attempt = 0; attempt != 2; ++attempt) {
            auto failed = probe();
            assert(RadioLibInterface::instance == failed.get());
            failed.reset();
            mainLoopPoll(); // Actual periodic poll must not dereference a deleted probe.
            assert(RadioLibInterface::instance == nullptr);
        }
    }
    auto older = probe();
    auto active = probe();
    older.reset();
    assert(RadioLibInterface::instance == active.get());
    mainLoopPoll();
    active.reset();
    assert(RadioLibInterface::instance == nullptr);
    puts("RadioLib failed-probe singleton lifetime tests: OK");
}
'''.replace("DESTRUCTOR_DECLARATION", declaration).replace(
        "CONSTRUCTOR", function(source, signatures[0])).replace("DESTRUCTOR", destructor).replace(
        "POLL", function(source, signatures[1]))
    binary = directory / "test-radio-lifetime"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-x", "c++", "-",
                    "-o", str(binary)], input=fixture, text=True, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if args.upstream_repro:
        assert result.returncode != 0 and "heap-use-after-free" in result.stderr, result.stderr
        print(result.stderr)
        print("Pinned upstream absent-radio use-after-free reproduced: OK")
    else:
        print(result.stdout, end="")
        if result.returncode:
            raise RuntimeError(result.stderr)
