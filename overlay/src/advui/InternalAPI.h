#pragma once

#include "mesh/PhoneAPI.h"

namespace advui
{

/**
 * In-process client API.
 *
 * The phone app talks to the node over the ToRadio/FromRadio protobuf API via a
 * PhoneAPI subclass (BLE / serial / TCP). Our on-device UI reuses the *same*
 * boundary, but in-process: subclassing PhoneAPI makes the engine feed us the
 * exact FromRadio stream a phone gets (config, node DB, live packets), and lets
 * us push ToRadio for outgoing messages/commands. See docs/M1-design.md.
 */
class InternalAPI : public PhoneAPI
{
  public:
    /// Register with the mesh service and begin the config download.
    /// Public wrapper: PhoneAPI::handleStartConfig() is protected.
    void begin() { handleStartConfig(); }

  protected:
    /// The UI shares the engine's process, so the link is always up.
    bool checkIsConnected() override { return true; }
};

} // namespace advui
