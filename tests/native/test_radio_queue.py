"""Check actual absent-radio queue status and the pinned Python client's sender."""
from pathlib import Path
import argparse
import collections
import os
import subprocess
import tempfile
from types import SimpleNamespace

from meshtastic import mesh_interface
from meshtastic.protobuf import mesh_pb2

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--upstream-repro", action="store_true")
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="adv-radio-queue-") as directory:
    directory = Path(directory)
    path = directory / "src/mesh/Router.cpp"
    path.parent.mkdir(parents=True)
    path.write_bytes(subprocess.check_output([
        "git", "-C", str(ROOT / "firmware"), "show", "HEAD:src/mesh/Router.cpp"]))
    if not args.upstream_repro:
        subprocess.run(["git", "apply", str(ROOT / "overlay/patches/router-no-radio-local-queue.patch")],
                       cwd=directory, check=True)
    source = path.read_text()
    method = function(source, "meshtastic_QueueStatus Router::getQueueStatus()")
    if not args.upstream_repro:
        assert method == function((ROOT / "firmware/src/mesh/Router.cpp").read_text(),
                                  "meshtastic_QueueStatus Router::getQueueStatus()")
    local_send = function(source, "ErrorCode Router::sendLocal(")
    assert local_send == function((ROOT / "firmware/src/mesh/Router.cpp").read_text(),
                                  "ErrorCode Router::sendLocal(")
    assert local_send.index("if (isToUs(p))") < local_send.index("else if (!iface)")
    assert "abortSendAndNak(meshtastic_Routing_Error_NO_INTERFACE, p);" in local_send

    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
constexpr uint32_t MAX_RX_FROMRADIO = 4;
using ErrorCode = int;
using RxSource = int;
constexpr int ERRNO_OK = 0, ERRNO_NO_INTERFACES = 1, RX_SRC_USER = 1;
constexpr int meshtastic_Routing_Error_NO_INTERFACE = 10;
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
struct meshtastic_MeshPacket {
    uint32_t to;
    uint8_t channel = 0, hop_limit = 0;
    bool pki_encrypted = false, want_ack = false;
};
struct meshtastic_NodeInfoLite { uint8_t channel; };
struct NodeDB {
    const meshtastic_NodeInfoLite *getMeshNode(uint32_t) { return nullptr; }
};
NodeDB database;
NodeDB *nodeDB = &database;
struct { struct { uint8_t hop_limit = 3; } lora; } config;
struct Default { static uint8_t getConfiguredOrDefaultHopLimit(uint8_t hops) { return hops ? hops : 3; } };
bool isToUs(const meshtastic_MeshPacket *p) { return p->to == 1; }
bool isBroadcast(uint32_t to) { return to == UINT32_MAX; }
void printPacket(const char *, const meshtastic_MeshPacket *) {}
struct meshtastic_QueueStatus { uint32_t res, mesh_packet_id, free, maxlen; };
struct LocalQueue {
    uint32_t available = MAX_RX_FROMRADIO;
    uint32_t numFree() { return available; }
};
struct Radio {
    meshtastic_QueueStatus getQueueStatus() { return {7, 9, 2, 16}; }
};
struct Router {
    Radio *iface = nullptr;
    LocalQueue fromRadioQueue;
    int enqueued = 0, rejected = 0, sent = 0, rejection = 0;
    meshtastic_QueueStatus getQueueStatus();
    ErrorCode sendLocal(meshtastic_MeshPacket *, RxSource);
    void enqueueReceivedMessage(meshtastic_MeshPacket *) { ++enqueued; }
    void abortSendAndNak(int reason, meshtastic_MeshPacket *) { ++rejected; rejection = reason; }
    void handleReceived(meshtastic_MeshPacket *, RxSource) {}
    ErrorCode send(meshtastic_MeshPacket *) { ++sent; return ERRNO_OK; }
};
METHOD
LOCAL_SEND
int main() {
    Router router;
    meshtastic_MeshPacket local{1}, remote{2}, broadcast{UINT32_MAX};
    assert(router.sendLocal(&local, RX_SRC_USER) == ERRNO_OK);
    assert(router.enqueued == 1 && router.sent == 0 && router.rejected == 0);
    assert(router.sendLocal(&remote, RX_SRC_USER) == ERRNO_NO_INTERFACES);
    assert(router.sendLocal(&broadcast, RX_SRC_USER) == ERRNO_NO_INTERFACES);
    assert(router.enqueued == 1 && router.sent == 0 && router.rejected == 2);
    assert(router.rejection == meshtastic_Routing_Error_NO_INTERFACE);
    for (uint32_t free = 0; free <= MAX_RX_FROMRADIO; ++free) {
        router.fromRadioQueue.available = free;
        auto qs = router.getQueueStatus();
        assert(qs.res == 0 && qs.mesh_packet_id == 0);
        EXPECT_LOCAL_QUEUE
    }
    router.fromRadioQueue.available = 3;
    auto qs = router.getQueueStatus();
    printf("%u %u\n", qs.free, qs.maxlen);
    Radio radio;
    router.iface = &radio;
    qs = router.getQueueStatus();
    assert(qs.res == 7 && qs.mesh_packet_id == 9 && qs.free == 2 && qs.maxlen == 16);
    assert(router.sendLocal(&remote, RX_SRC_USER) == ERRNO_OK && router.sent == 1);
}
'''.replace("METHOD", method).replace("LOCAL_SEND", local_send).replace("EXPECT_LOCAL_QUEUE", "" if args.upstream_repro else
                                       "assert(qs.free == free && qs.maxlen == MAX_RX_FROMRADIO);")
    binary = directory / "test-radio-queue"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-x", "c++", "-",
                    "-o", str(binary)], input=fixture, text=True, check=True)
    status = subprocess.check_output([str(binary)], text=True)
    free, capacity = map(int, status.split())


class QueueWait(RuntimeError):
    pass


def run_client(*, already_received_status):
    client = mesh_interface.MeshInterface(noProto=True)
    client.noProto = False
    client.queue = collections.OrderedDict()
    client.queueStatus = (mesh_pb2.QueueStatus(free=free, maxlen=capacity)
                          if already_received_status else None)
    packet = mesh_pb2.ToRadio()
    packet.packet.id = 42
    sent = []

    def send(message):
        sent.append(message.packet.id)
        # Reproduce an early status while _sendToRadioImpl still owns the packet.
        client._handleQueueStatusFromRadio(mesh_pb2.QueueStatus(
            free=free, maxlen=capacity, mesh_packet_id=message.packet.id))

    def blocked(_seconds):
        raise QueueWait("client waits for a permanently zero-capacity RF queue")

    client._sendToRadioImpl = send
    original_time = mesh_interface.time
    mesh_interface.time = SimpleNamespace(sleep=blocked)
    try:
        try:
            client._sendToRadio(packet)
        except QueueWait:
            return True, sent
        return False, sent
    finally:
        mesh_interface.time = original_time


for received in (False, True):
    stalled, sent = run_client(already_received_status=received)
    assert stalled == args.upstream_repro, (received, stalled, sent)
    if not args.upstream_repro:
        assert sent == [42]
print("Pinned upstream zero-capacity API stall reproduced: OK" if args.upstream_repro else
      "Absent-radio local API queue and real Python sender regression: OK")
