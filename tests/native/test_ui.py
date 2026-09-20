"""Behavioral regressions using verbatim production UI functions and host mocks."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = (ROOT / "overlay/src/advui/AdvUI.cpp").read_text()


def section(start: str, end: str) -> str:
    return SOURCE.split(start, 1)[1].split(end, 1)[0]


conversations = "void AdvUI::buildConversations()" + section(
    "void AdvUI::buildConversations()", "void AdvUI::openConv"
)
node_list = "int AdvUI::buildNodeList" + section(
    "int AdvUI::buildNodeList", "// Fills chanList[]"
)
node_less = "bool nodeLess" + section("bool nodeLess", "// Trim s in place")
chat_keys = section(
    "    // Home: recent conversations. Typing opens node search; Tab switches to all nodes.",
    "    // Home node list: navigable directly",
)

CPP = r'''
#include "AdvOrder.h"
#include "AdvUtf8.h"
using namespace advui;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>
constexpr uint32_t NODENUM_BROADCAST = 0xffffffff;
struct Msg { uint32_t from, to; uint8_t ch; };
Msg g_msgs[32]; int g_msgCount = 0;
uint32_t myNodeNum() { return 1; }
bool g_radioCompanion = false, g_nameShort = false, g_ruMode = false, g_uiCfgPortable = false;
uint8_t g_nodeSort = 0, g_favChannels = 0;
struct meshtastic_NodeInfoLite {
    uint32_t num;
    uint32_t last_heard = 0;
    bool has_hops_away = true;
    uint8_t hops_away = 0;
    bool is_favorite = false;
    std::string name;
};
struct ReviewNodeDb {
    std::vector<meshtastic_NodeInfoLite> nodes;
    size_t getNumMeshNodes() const { return nodes.size(); }
    meshtastic_NodeInfoLite *getMeshNodeByIndex(size_t i) {
        return i < nodes.size() ? &nodes[i] : nullptr;
    }
    void saveToDisk(int) { saves++; }
    int saves = 0;
} reviewDb;
constexpr int SEGMENT_NODEDATABASE = 1;
ReviewNodeDb *nodeDB = &reviewDb;
struct CompNode { uint32_t num, lastHeard; char longName[24], shortName[5]; uint8_t hops = 255; };
constexpr int kMaxCompNodes = 64;
CompNode companion[kMaxCompNodes];
std::atomic<int> g_compNodeCount{0};
bool bleCopyCompNodeAt(int i, CompNode *out) {
    if (i < 0 || i >= g_compNodeCount) return false;
    *out = companion[i]; return true;
}
bool hasUnreadFrom(uint32_t) { return false; }
bool nodeIsFavorite(const meshtastic_NodeInfoLite *n) { return n && n->is_favorite; }
const char *nodeName(const meshtastic_NodeInfoLite *n) { return n->name.c_str(); }
bool ciContains(const char *hay, const char *needle) { return std::strstr(hay, needle); }
void persistUiCfg() {}
void markMsgsDirty() {}
struct meshtastic_User {
    char long_name[40], short_name[5];
    bool is_licensed, has_is_unmessagable, is_unmessagable;
};
struct meshtastic_AdminMessage { int which_payload_variant; meshtastic_User set_owner; };
#define meshtastic_AdminMessage_init_default {}
constexpr int meshtastic_AdminMessage_set_owner_tag = 1;
meshtastic_User companionOwner{}, sentOwner{};
bool ownerReady = false;
int ownerWrites = 0;
std::atomic<uint32_t> g_linkMyNode{1};
bool bleCopyCompOwner(meshtastic_User *out) {
    if (!ownerReady) return false;
    *out = companionOwner; return true;
}
bool sendAdminToNode(const meshtastic_AdminMessage &admin) {
    assert(admin.which_payload_variant == meshtastic_AdminMessage_set_owner_tag);
    sentOwner = admin.set_owner; ownerWrites++; return true;
}
void bleUpdateCompNodeNames(uint32_t, const char *, const char *) {}
struct AdvKeyboard {
    static constexpr uint8_t kInputHelp = 0x0f, kLongEsc = 0x1c;
};
class AdvUI {
public:
    enum Mode { MODE_CHATS, MODE_NODES, MODE_PICKER, MODE_SETNAME, MODE_SETTINGS, MODE_COMPOSE, MODE_NODE, MODE_EMOJI,
                MODE_INPUTHELP, MODE_REBOOT, MODE_BTPIN, MODE_BLEPIN, MODE_BLESCAN };
    Mode inputHelpReturn = MODE_CHATS;
    uint8_t inputHelpPage = 0;
    void openInputHelp();
    void handleHelpKey(unsigned char);
    enum class SendFailure { NONE };
    SendFailure sendFailure = SendFailure::NONE;
    SendFailure sendChannel(int, const char *, uint32_t) { return SendFailure::NONE; }
    SendFailure sendMessage(uint32_t, const char *, uint32_t) { return SendFailure::NONE; }
    uint32_t selectedNum = 100, pendingReplyId = 0;
    int selectedChannel = -1, chatScroll = 0, emojiSel = 0;
    Mode emojiReturn = MODE_COMPOSE;
    char msgBuf[200]{};
    uint8_t msgLen = 0;
    void handleComposeKey(unsigned char);
    struct Conv { bool isChan; uint8_t ch; uint32_t node; int lastIdx, order; };
    static constexpr int kMaxConv = 32;
    Conv conv[kMaxConv]{};
    int convCount = 0, sel = 0, scrollTop = 0, queryLen = 0;
    char query[24]{};
    bool confirmDel = false;
    Conv pendingDelete{};
    char nameBuf[65]{};
    uint8_t nameLen = 0;
    char pendingLat = 0;
    int editTarget = 20, applied = 0;
    Mode nameReturn = MODE_SETTINGS;
    bool applyName() { applied++; return false; }
    bool renameCompanion();
    void handleEditorKey(unsigned char);
    Mode mode = MODE_CHATS;
    uint32_t deleted = 0;
    int deletedChannel = -1;
    void buildConversations();
    int buildNodeList(uint16_t *, int, const char *);
    void handleChatKey(unsigned char);
    void deleteConversation(const Conv &c) { deleted = c.node; deletedChannel = c.isChan ? c.ch : -1; }
    void openConv(int) {}
    void favNode(uint32_t, bool) {}
};
'''
CPP += "constexpr int kMaxFavNodes" + section("constexpr int kMaxFavNodes", "// Case-insensitive substring")
CPP += "void migrateDbFavourites()" + section("void migrateDbFavourites()", "const EnumOpt kRoleOpts2")
CPP += node_less + node_list + conversations
CPP += r'''
void AdvUI::handleChatKey(unsigned char c) {
    bool bksp = c == 8, enter = c == 13, up = c == 0xb5, down = c == 0xb6;
    bool left = c == 0xb4, right = c == 0xb7, tab = c == 9;
    bool printable = c >= 0x20 && c < 0x7f;
'''
CPP += chat_keys + "\n}\n"
CPP += "uint16_t translitSingle" + section("uint16_t translitSingle", "constexpr int kEmoteAdv")
editor = section("    if (mode == MODE_SETNAME) {", "    if (mode == MODE_BTPIN) {")
CPP += r"""
unsigned editMax(int target) { return target == 1 ? 4 : 63; }
void AdvUI::handleEditorKey(unsigned char c) {
    bool esc = c == 27, enter = c == 13, bksp = c == 8;
    bool printable = c >= 0x20 && c < 0x7f;
    if (mode == MODE_SETNAME) {
"""
CPP += editor + "\n}\n"
CPP += r"""
const char *kEmojiPalette[] = {"👍"};
constexpr int kEmojiCount = 1;
void AdvUI::handleComposeKey(unsigned char c) {
    bool esc = c == 27, enter = c == 13, bksp = c == 8, tab = c == 9;
    bool left = c == 0xb4, right = c == 0xb7, up = c == 0xb5, down = c == 0xb6;
    bool printable = c >= 0x20 && c < 0x7f;
    if (mode == MODE_EMOJI) {
"""
CPP += section("    if (mode == MODE_EMOJI) {", "    // Home: recent conversations.") + "\n}\n"
CPP += "void AdvUI::openInputHelp()" + section("void AdvUI::openInputHelp()", "void AdvUI::drawInputHelp()")
CPP += r"""
void AdvUI::handleHelpKey(unsigned char c) {
    bool esc = c == 27, enter = c == 13, tab = c == 9;
    bool left = c == 0xb4, right = c == 0xb7, up = c == 0xb5, down = c == 0xb6;
    if (mode == MODE_INPUTHELP) {
"""
CPP += section("    if (mode == MODE_INPUTHELP) {", "    if (c == AdvKeyboard::kLongEsc) {") + "\n}\n"
rename = SOURCE.split("    // name (long / short): applied live, no reboot", 1)[1].split(
    "    if (editTarget == 1) {\n        utf8CopyValid(owner.short_name", 1
)[0]
CPP += "bool AdvUI::renameCompanion() {\n" + rename + "\nreturn false;\n}\n"
CPP += r'''
int main() {
    AdvUI ui;
    // Exercise the exact production transducer, including all letter cases.
    const auto transliterate = [](const char *keys, size_t cap = 200) {
        char buf[200]{}; uint8_t len = 0; char pending = 0;
        for (const char *key = keys; *key; ++key) {
            translitFeed(buf, len, cap, *key, pending);
            assert(len < cap && buf[len] == 0 && strlen(buf) == len);
            for (const char *p = buf; *p;) {
                const auto rune = utf8Decode(p);
                assert(rune.valid);
                p += rune.bytes;
            }
        }
        return std::string(buf);
    };
    const char *pairs[][2] = {
        {"Schyot", "Счёт"}, {"schast'e", "счастье"}, {"rasschitat'", "рассчитать"},
        {"shchuka", "щука"}, {"Shchuka", "Щука"}, {"SHCHUKA", "ЩУКА"},
        {"wuka", "щука"}, {"Wuka", "Щука"}, {"WUKA", "ЩУКА"},
        {"Ashhabad", "Ашхабад"}, {"shh", "шх"}, {"shhh", "шхх"},
        {"SCH", "СЧ"}, {"sCh", "сЧ"}, {"sch", "сч"}, {"shch", "щ"},
        {"sh ch", "ш ч"}, {"sh-ch", "ш-ч"}, {"sh!ch", "ш!ч"},
        {"zh sh ch ya yu yo ye y j x ' h c e q", "ж ш ч я ю ё э ы й ъ ь х ц е я"},
        {"YA YU YO YE", "Я Ю Ё Э"}, {"schshchsch", "счщсч"},
    };
    for (const auto &pair : pairs) assert(transliterate(pair[0]) == pair[1]);
    bool alphabet[64]{};
    for (const auto &page : kTranslitHelp) {
        for (const auto &entry : page) {
            const std::string value(entry);
            const size_t colon = value.find(':');
            assert(colon != std::string::npos);
            assert(transliterate(value.substr(0, colon).c_str()) == value.substr(colon + 1));
            const auto cp = utf8Decode(value.c_str() + colon + 1).codepoint;
            if (cp >= 0x430 && cp <= 0x451) alphabet[cp - 0x430] = true;
        }
    }
    for (unsigned cp = 0x430; cp <= 0x44f; ++cp) assert(alphabet[cp - 0x430]);
    assert(alphabet[0x451 - 0x430]); // ё is outside the contiguous alphabet range
    assert(transliterate("sh", 3) == "ш"); // morph even when the buffer is full
    assert(transliterate("w", 3) == "щ");
    assert(transliterate("sch", 5) == "сч");
    assert(transliterate("shch", 5) == "щ");
    assert(transliterate("asch", 3) == "а"); // rejected c cannot replace а
    assert(transliterate("asch", 4) == "а");
    assert(transliterate("sch", 1).empty());
    for (size_t cap = 1; cap <= 200; cap++) {
        const std::string keys = std::string(200, 'a') + "sch shch yo 123!";
        transliterate(keys.c_str(), cap);
    }
    g_ruMode = true;
    ui.mode = AdvUI::MODE_SETNAME; ui.editTarget = 1;
    for (char c : std::string("sch")) ui.handleEditorKey(c);
    assert(std::string(ui.nameBuf) == "сч" && ui.nameLen == 4);
    ui.handleEditorKey(8);
    assert(std::string(ui.nameBuf) == "с" && !ui.pendingLat);
    for (char c : std::string("ch")) ui.handleEditorKey(c);
    assert(std::string(ui.nameBuf) == "сч");
    ui = {}; ui.mode = AdvUI::MODE_COMPOSE;
    for (char c : std::string(98, 'a') + "sh") ui.handleComposeKey(c);
    assert(ui.msgLen == 198 && std::string(ui.msgBuf).substr(196) == "ш");
    ui.handleComposeKey('c'); ui.handleComposeKey('h');
    assert(ui.msgLen == 198 && std::string(ui.msgBuf).substr(196) == "ш");
    ui.handleComposeKey('.'); ui.handleComposeKey('s'); ui.handleComposeKey('h');
    assert(ui.msgLen == 199 && ui.msgBuf[198] == '.');
    ui = {}; ui.mode = AdvUI::MODE_COMPOSE;
    for (char c : std::string("shch")) ui.handleComposeKey(c);
    assert(std::string(ui.msgBuf) == "щ");
    ui.handleComposeKey(8); ui.handleComposeKey('s');
    ui.handleComposeKey(9); ui.handleComposeKey(13); ui.handleComposeKey('h');
    assert(std::string(ui.msgBuf) == "с👍х"); // never splice through an emoji
    ui.handleComposeKey(8);
    assert(std::string(ui.msgBuf) == "с👍");
    ui.handleComposeKey(8);
    assert(std::string(ui.msgBuf) == "с" && !ui.pendingLat);
    ui.handleComposeKey(8); ui.handleComposeKey('s');
    ui.pendingReplyId = 123;
    const char savedPending = ui.pendingLat;
    ui.handleHelpKey(0x0f);
    assert(ui.mode == AdvUI::MODE_INPUTHELP && ui.inputHelpPage == 0);
    ui.handleHelpKey(0xb7);
    assert(ui.inputHelpPage == 1);
    ui.handleHelpKey(0xb7);
    assert(ui.inputHelpPage == 1);
    ui.handleHelpKey(0xb4);
    assert(ui.inputHelpPage == 0);
    ui.handleHelpKey('a'); ui.handleHelpKey(8);
    assert(std::string(ui.msgBuf) == "с" && ui.pendingLat == savedPending && ui.pendingReplyId == 123);
    ui.handleHelpKey(27);
    assert(ui.mode == AdvUI::MODE_COMPOSE);
    ui.handleComposeKey('h');
    assert(std::string(ui.msgBuf) == "ш");
    for (auto mode : {AdvUI::MODE_CHATS, AdvUI::MODE_SETNAME, AdvUI::MODE_SETTINGS, AdvUI::MODE_EMOJI}) {
        ui.mode = mode;
        ui.handleHelpKey(0x0f); ui.handleHelpKey(13);
        assert(ui.mode == mode && ui.applied == 0 && ownerWrites == 0);
    }
    for (auto mode : {AdvUI::MODE_REBOOT, AdvUI::MODE_BTPIN, AdvUI::MODE_BLEPIN, AdvUI::MODE_BLESCAN}) {
        ui.mode = mode; ui.handleHelpKey(0x0f);
        assert(ui.mode == mode);
    }
    g_ruMode = false;
    ui = {};
    assert(!setFavNodeLocal(0, true));
    g_msgs[0] = {100, 1, 0}; g_msgCount = 1;
    ui.handleChatKey(8);
    assert(ui.confirmDel && ui.conv[ui.sel].node == 100);
    g_msgs[g_msgCount++] = {200, 1, 0};
    ui.handleChatKey(13);
    assert(ui.deleted == 100 && !ui.confirmDel);
    // Channel identities are independent of DM peer numbers and arrival order.
    ui = {};
    g_msgs[0] = {100, NODENUM_BROADCAST, 2}; g_msgCount = 1;
    ui.handleChatKey(8);
    g_msgs[g_msgCount++] = {200, NODENUM_BROADCAST, 3};
    ui.buildConversations(); // draw-time rebuild must retain target highlighting
    assert(ui.conv[ui.sel].ch == 2);
    ui.handleChatKey(13);
    assert(ui.deletedChannel == 2);
    ui = {};
    g_msgs[0] = {100, 1, 0}; g_msgCount = 1;
    ui.handleChatKey(8);
    g_msgs[0] = {200, 1, 0}; // original evicted
    ui.handleChatKey(13);
    assert(!ui.confirmDel && ui.deleted == 0);

    reviewDb.nodes.push_back({1, 200, true, 0, false, "Self"});
    for (uint32_t i = 2; i <= 149; i++)
        reviewDb.nodes.push_back({i, 200-i, true, 1, false, "Zulu"});
    reviewDb.nodes.push_back({150, 0, true, 0, false, "Aaron"});
    g_nodeSort = 2;
    uint16_t out[150];
    assert(ui.buildNodeList(out, 150, "") == 149);
    assert(reviewDb.nodes[out[0]].num == 150);
    assert(ui.buildNodeList(out, 128, "") == 128);
    assert(reviewDb.nodes[out[0]].num == 150); // truncate AFTER ordering

    g_radioCompanion = true;
    companion[0] = {10, 200, "Zulu", "AA", 2};
    companion[1] = {20, 100, "Aaron", "ZZ", 0};
    g_compNodeCount = 2;
    setFavNodeLocal(20, true);
    const uint32_t first[] = {20, 10, 20, 20};
    for (g_nodeSort = 0; g_nodeSort < 4; g_nodeSort++) {
        assert(ui.buildNodeList(out, 150, "") == 2);
        assert(companion[out[0]].num == first[g_nodeSort]);
    }
    g_nodeSort = 2;
    g_nameShort = true;
    assert(ui.buildNodeList(out, 150, "") == 2 && companion[out[0]].num == 10);
    assert(ui.buildNodeList(out, 150, "ZZ") == 1 && companion[out[0]].num == 20);

    // Portable IDs, not stale flags from a different install, are authoritative.
    g_radioCompanion = false;
    g_uiCfgPortable = true;
    reviewDb.nodes[1].is_favorite = true; // not in portable set
    migrateDbFavourites();
    assert(!reviewDb.nodes[1].is_favorite);
    assert(reviewDb.nodes[19].num == 20 && reviewDb.nodes[19].is_favorite);
    assert(g_favNodeCount == 1 && reviewDb.saves == 1);
    assert(setFavNodeLocal(20, false));
    assert(!isFav(&reviewDb.nodes[19])); // no sticky UI OR of stale DB flag
    g_favouritesReady = false;
    g_uiCfgPortable = false; // legacy install imports instead of erasing its flags
    migrateDbFavourites();
    assert(favNodeLocal(20));

    // Actual editor branch must invoke applyName even when credentials are empty.
    for (int target : {20, 21, 22, 23, 24}) {
        ui.mode = AdvUI::MODE_SETNAME;
        ui.editTarget = target;
        ui.applied = 0;
        ui.nameLen = 0;
        ui.handleEditorKey(13);
        assert(ui.applied == 1 && ui.mode == AdvUI::MODE_SETTINGS);
    }
    ui.mode = AdvUI::MODE_SETNAME;
    ui.editTarget = 0; ui.applied = 0;
    ui.handleEditorKey(13);
    assert(ui.applied == 0); // empty owner names remain forbidden
    g_radioCompanion = true;
    ui.editTarget = 1;
    std::strcpy(ui.nameBuf, "New");
    ui.renameCompanion();
    assert(ownerWrites == 0); // no partial snapshot, no set_owner
    ownerReady = true;
    std::strcpy(companionOwner.long_name, "An owner name longer than 24 bytes");
    companionOwner.is_licensed = true;
    companionOwner.has_is_unmessagable = true;
    companionOwner.is_unmessagable = true;
    ui.renameCompanion();
    assert(ownerWrites == 1 && !std::strcmp(sentOwner.short_name, "New"));
    assert(!std::strcmp(sentOwner.long_name, companionOwner.long_name));
    assert(sentOwner.is_licensed && sentOwner.has_is_unmessagable && sentOwner.is_unmessagable);
    std::puts("production UI behavior tests: OK");
}
'''

with tempfile.TemporaryDirectory(prefix="meshtastic-ui-test-") as temp:
    binary = str(Path(temp) / "repro-ui")
    subprocess.run(
        ["c++", "-I" + str(ROOT / "overlay/src/advui"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-x", "c++", "-",
         str(ROOT / "overlay/src/advui/AdvUtf8.cpp"), "-o", binary],
        input=CPP, text=True, check=True,
    )
    subprocess.run([binary], check=True)
