# Interface — the on-device app

The advui firmware replaces the stock Meshtastic screen with a keyboard-first UI built
for the Cardputer ADV. This is the guide to what's on screen and how to drive it.

The engine underneath is unmodified upstream Meshtastic, so the node still behaves like a
normal Meshtastic device on the mesh — this only changes the local UI. In **companion
mode** the same UI drives a *different* node's radio over Bluetooth instead (see the end).

## Screens

### Splash

On boot you get a branded **`Meshtastic ADV`** splash for ~2 seconds (or until you press a
key) while the mesh engine comes up. Then it drops into Chats.

### Chats (home)

Your recent conversations — DMs and channels mixed, newest first.

```
 Chats                     [2 new]  78%    <- title · unread badge · our battery
 ------------------------------------------
 ✉ SPb Gate                        14:02   <- unread envelope · name · last-msg time
   hi, anyone near Nevsky?                 <- preview of the last message
 # MediumFast                      13:47
   KSV: back online
   Neva Bridge                     12:30
   > see you there                         <- ">" = the last message is ours
 type find   Tab: all nodes
```

- **Row = conversation.** Channels are prefixed `#` (cyan), favourites are yellow, a red
  envelope marks unread. The preview shows the sender's short name in channels and `>` when
  the last word was yours.
- **Enter** opens the conversation **at the first unread message**.
- **← / →** un-favourite / favourite the selected contact or channel.
- **Typing** searches *all* nodes to start a new chat; **Tab** switches to the full node list.
- In companion mode a small **Bluetooth rune** next to the title shows the link state
  (green = linked, yellow = reconnecting, red = down).

### Node list (Tab)

Everyone the node DB knows, one row per node.

```
 42 nodes                  [2 new]  78%    <- node count (200+ at the DB cap) · battery
 ------------------------------------------
 ✉ ksv-relay      ▂▄▆_   →1   4m    CLI
 * SPb Gate       ▂▄▆█   →0   now   RTR    <- * favourite (yellow)
   Neva Bridge    ▁___   →3   12m   RTR
 </>fav  ENTER open  type find  ESC/Tab chats
```

- **Sort:** unread first → favourites → nodes you talk to → everyone else by hop distance.
- **↑ / ↓** move (the list scrolls), **Enter** opens the node, **← / →** manage favourites,
  **typing** filters by name, **ESC** or **Tab** returns to Chats.

Reading a row:

| Field       | Meaning                                                                        |
| ----------- | ------------------------------------------------------------------------------ |
| **name**    | long name (falls back to short name, then `!nodenum`); yellow = favourite       |
| **signal**  | 0–4 bars from the last direct SNR (green strong → orange weak); empty = heard only via relays |
| **`→N`**    | hops away: `→0` direct neighbour · `→?` unknown                                 |
| **age**     | last heard: `now` · `5m` · `2h` · `3d`; green fresh, grey stale                 |
| **role**    | `CLI` client · `RTR` router · `RPT` repeater · `TRK` tracker · `SEN` sensor · `TAK` |

### Conversation

The thread view — a DM or a channel. The header repeats the node/channel row, messages fill
the middle, the compose bar sits at the bottom.

```
 SPb Gate         ▂▄▆█   →0   now   RTR
 ------------------------------------------
 13:58 > Are you around?             ✓     <- our message · delivery check
 14:02 < yes! near the bridge              <- their message
       ↳ Are you around?                   <- it quotes ours (reply)
 14:03 > 👍
 _                                    RU   <- compose bar · input-mode badge
 type  Tab emoji  <reply  >react
```

- **Every message** carries a compact local `HH:MM` (set your UTC offset in Settings) and a
  direction mark: `>` sent, `<` received. In channels received messages also show the
  sender's short name.
- **Delivery status** on your messages: a grey dot while sending → a **green ✓** on the
  routing ACK (channel broadcasts get their ✓ on transmit) → a **red ✗ with the reason** on
  failure. When the newest message has failed, **Enter resends it**.
- **↑ / ↓** scroll the history; opening a thread auto-jumps to the first unread.
- **Type** and hit **Enter** to send. **Fn+L** toggles the Cyrillic transliteration layer
  (the `RU`/`EN` badge on the compose bar; the choice persists). **Tab** opens the emoji
  palette (~24 icons, arrows + Enter to insert; emoji render inline in text).
- **← / →** enter *message pick* mode: ↑/↓ choose a received message, then
  - **→ reaction** — a quick strip of tapbacks (`</>` pick, Enter sends). Reactions from
    others (including phone apps) show under the message they refer to.
  - **← reply** — quotes the picked message in your next send; the thread shows the quote
    above your message, phone apps render it as a proper reply.
  - Messages from very old history (pre-reactions builds) have no packet id and can't be
    reacted to — the footer says so.
- **ESC** goes back. The footer's right corner always shows **our own battery**.

### Settings (long-press ESC anywhere)

| Row           | What it does                                                                |
| ------------- | --------------------------------------------------------------------------- |
| **Name / Short** | the node's long and short names (text editor: type, Enter saves)         |
| **Region**    | LoRa region picker — required on first boot                                  |
| **Preset**    | modem preset (LongFast, MediumFast, …)                                       |
| **Frequency** | frequency-slot override in MHz                                               |
| **Channel**   | primary channel name + PSK                                                   |
| **UTC**       | UTC-offset picker with city labels — drives all timestamps                   |
| **WiFi**      | join a network (NTP time comes with it); enabling WiFi turns Bluetooth off   |
| **MQTT**      | bridge the mesh to the internet: default public broker or your own           |
| **Radio**     | **Onboard (Cap LoRa)** or **Companion via BLE** — see below                  |

**↑/↓** move, **Enter** edits (or toggles), **ESC** backs out. Changes that affect the radio
(Region / Preset / Frequency / Channel, WiFi / MQTT, Radio mode) reboot the device to apply.

### Companion mode (Settings → Radio → Companion via BLE)

The Cardputer becomes a keyboard + screen terminal for **another, stock Meshtastic node**
(Heltec, T-Beam, RAK…) over Bluetooth — its own LoRa cap is not needed. The other node keeps
its normal firmware; the Cardputer talks to the same BLE client API the phone app uses.

1. **Scan** — after the reboot you land in *Find node*: nearby Meshtastic nodes with names
   and signal. `↑/↓` choose, **Enter** connects, **R** rescans.
2. **Pairing** — the first connect asks for the **PIN shown on the node's screen** (digits,
   Enter). The bond is remembered; later connects are automatic, including after reboots.
3. **Linked** — the node's contacts and channels download and you land in Chats. Everything
   above — PKI-encrypted DMs, delivery checkmarks, reactions, replies, channels — now goes
   through the linked node's radio.

While linked, the **Bluetooth rune** in the Chats header tracks the connection, and
**Settings → Radio** becomes a status page: link state, packets received, the node's id,
link signal (dB), **the node's battery**, and how many nodes have synced. **R** forces a
reconnect, **F** forgets the node (back to scan), **ESC** just leaves — the link stays up.
Drops auto-reconnect in the background.

> 📱 The node has a single Bluetooth slot — close the Meshtastic phone app while the
> Cardputer is linked, or they'll steal the connection from each other.

## Odds and ends

- **History survives reboots** — the message ring (with reactions and reply links) is
  persisted to flash, as are all settings.
- **Sound + light:** one beep + green LED flash for favourites, a blue flash for everyone
  else — never a buzz per packet.
- The node DB caps at 200 nodes on this hardware; the header shows `200+` when it's full.
  In companion mode the synced node table holds the 64 most relevant nodes.
- Other nodes' battery levels aren't stored by this build's compact node DB — only our own
  battery (header/footer) and, in companion mode, the linked node's.
