"""Apply the shipped guard to the pinned upstream function and execute it.

Run in the firmware CI job, which checks out the submodule. No USB or network.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
upstream = subprocess.check_output(
    ["git", "-C", str(ROOT / "firmware"), "show", "HEAD:src/mesh/NodeDB.cpp"], text=True
)
with tempfile.TemporaryDirectory(prefix="adv-node-admission-") as directory:
    temp = Path(directory)
    source_file = temp / "src/mesh/NodeDB.cpp"
    source_file.parent.mkdir(parents=True)
    source_file.write_text(upstream)
    for patch in ("nodedb-profile-sync", "nodedb-favourites-reconcile", "nodedb-contact-profile",
                  "nodedb-full-pinned-guard"):
        subprocess.run(
            ["git", "apply", str(ROOT / f"overlay/patches/{patch}.patch")], cwd=temp, check=True,
        )
    patched = source_file.read_text()
    function = "meshtastic_NodeInfoLite *NodeDB::getOrCreateMeshNode(NodeNum n)" + patched.split(
        "meshtastic_NodeInfoLite *NodeDB::getOrCreateMeshNode(NodeNum n)", 1
    )[1].split("/// Sometimes we will have Position objects", 1)[0]
    function += "void NodeDB::set_favorite" + patched.split("void NodeDB::set_favorite", 1)[1].split(
        "bool NodeDB::isFavorite", 1
    )[0]
    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#define ADVUI_PROFILE_SYNC 1
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
using NodeNum = uint32_t;
constexpr uint32_t NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK = 1;
std::map<uint32_t, bool> portable;
namespace advui {
    void advuiFavouriteChanged(uint32_t node, bool favourite) { portable[node] = favourite; }
    bool advuiRestoreFavourite(uint32_t node, bool current) {
        return portable.count(node) ? portable[node] : current;
    }
}
struct meshtastic_NodeInfoLite {
    uint32_t num, last_heard, bitfield;
    bool is_favorite, is_ignored;
    struct { struct { unsigned size; } public_key; } user;
};
class NodeDB {
public:
    std::vector<meshtastic_NodeInfoLite> storage;
    std::vector<meshtastic_NodeInfoLite> *meshNodes = &storage;
    uint16_t numMeshNodes; // pinned nanopb pb_size_t
    explicit NodeDB(size_t capacity) : storage(capacity), numMeshNodes(capacity) {
        for (size_t i = 0; i < capacity; i++)
            storage[i] = {uint32_t(i + 1), uint32_t(i + 1), 0, true, false, {{0}}};
    }
    bool isFull() { return numMeshNodes >= storage.size(); }
    meshtastic_NodeInfoLite *getMeshNode(NodeNum n) {
        for (size_t i = 0; i < numMeshNodes; i++)
            if (storage[i].num == n) return &storage[i];
        return nullptr;
    }
    meshtastic_NodeInfoLite *getOrCreateMeshNode(NodeNum n);
    void set_favorite(bool, uint32_t);
    void sortMeshDB() {}
    void saveNodeDatabaseToDisk() {}
};
'''
    fixture += function + r'''
int main() {
    for (size_t capacity : {32, 100, 150}) {
        NodeDB db(capacity);
        assert(db.getOrCreateMeshNode(1) == &db.storage[0]);
        assert(db.getOrCreateMeshNode(999) == nullptr);
        assert(db.numMeshNodes == capacity && db.storage.back().num == capacity);
        db.storage[1].is_favorite = false;
        auto *created = db.getOrCreateMeshNode(999);
        assert(created && created->num == 999 && db.numMeshNodes == capacity);
        assert(!db.getMeshNode(2) && db.getMeshNode(1));
        portable[999] = true;
        created->is_favorite = false;
        db.set_favorite(false, 999); // false-to-false still unstars the profile
        assert(!portable[999]);
        portable[888] = true;
        auto *restored = db.getOrCreateMeshNode(888);
        assert(restored && restored->is_favorite); // late-discovered portable ID
        portable.clear();
    }
    std::puts("pinned NodeDB admission tests: OK");
}
'''
    binary = temp / "test-node-admission"
    subprocess.run(
        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-x", "c++", "-", "-o", str(binary)],
        input=fixture, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
