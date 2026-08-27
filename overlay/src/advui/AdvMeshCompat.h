#pragma once

#include "mesh/NodeDB.h"
#include <cstdio>

namespace advui
{

// Meshtastic 2.7 keeps UserLite nested in NodeInfoLite. The 2.8 development
// line flattens the frequently-read identity fields to shrink NodeDB. Keep that
// storage-layout difference at one boundary instead of leaking version checks
// through the renderer and message sender.
#ifdef meshtastic_NodeInfoLite_user_tag
inline const char *nodeShortName(const meshtastic_NodeInfoLite *node)
{
    return node && node->has_user ? node->user.short_name : "";
}

inline const char *nodeLongName(const meshtastic_NodeInfoLite *node)
{
    return node && node->has_user ? node->user.long_name : "";
}

inline meshtastic_Config_DeviceConfig_Role nodeRole(const meshtastic_NodeInfoLite *node)
{
    return node && node->has_user ? node->user.role : meshtastic_Config_DeviceConfig_Role_CLIENT;
}

inline bool nodeIsFavorite(const meshtastic_NodeInfoLite *node) { return node && node->is_favorite; }

inline bool nodeIsUnmessagable(const meshtastic_NodeInfoLite *node)
{
    return node && node->has_user && node->user.has_is_unmessagable && node->user.is_unmessagable;
}

inline bool nodeHasPublicKey(const meshtastic_NodeInfoLite *node)
{
    return node && node->has_user && node->user.public_key.size == 32;
}

inline void nodeSetNames(meshtastic_NodeInfoLite *node, const char *longName, const char *shortName)
{
    if (!node)
        return;
    node->has_user = true;
    snprintf(node->user.long_name, sizeof(node->user.long_name), "%s", longName ? longName : "");
    snprintf(node->user.short_name, sizeof(node->user.short_name), "%s", shortName ? shortName : "");
}
#else
inline const char *nodeShortName(const meshtastic_NodeInfoLite *node) { return node ? node->short_name : ""; }
inline const char *nodeLongName(const meshtastic_NodeInfoLite *node) { return node ? node->long_name : ""; }

inline meshtastic_Config_DeviceConfig_Role nodeRole(const meshtastic_NodeInfoLite *node)
{
    return node ? node->role : meshtastic_Config_DeviceConfig_Role_CLIENT;
}

inline bool nodeIsFavorite(const meshtastic_NodeInfoLite *node)
{
    return node && (node->bitfield & NODEINFO_BITFIELD_IS_FAVORITE_MASK);
}

inline bool nodeIsUnmessagable(const meshtastic_NodeInfoLite *node) { return nodeInfoLiteIsUnmessagable(node); }
inline bool nodeHasPublicKey(const meshtastic_NodeInfoLite *node) { return node && node->public_key.size == 32; }

inline void nodeSetNames(meshtastic_NodeInfoLite *node, const char *longName, const char *shortName)
{
    if (!node)
        return;
    snprintf(node->long_name, sizeof(node->long_name), "%s", longName ? longName : "");
    snprintf(node->short_name, sizeof(node->short_name), "%s", shortName ? shortName : "");
    node->bitfield |= NODEINFO_BITFIELD_HAS_USER_MASK;
}
#endif

inline bool nodeHasName(const meshtastic_NodeInfoLite *node)
{
    return node && (nodeShortName(node)[0] || nodeLongName(node)[0]);
}

} // namespace advui
