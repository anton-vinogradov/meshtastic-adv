# Interface — the on-device app

The advui firmware replaces the stock Meshtastic screen with a keyboard-first UI built
for the Cardputer ADV. This is the guide to what's on screen and how to drive it.

The engine underneath is unmodified upstream Meshtastic, so the node still behaves like a
normal Meshtastic device on the mesh — this only changes the local UI.

## Screens

### Splash

On boot you get a branded **`Meshtastic ADV`** splash for ~2 seconds (or until you press any
key) while the mesh engine comes up and the node DB fills in. Then it drops to the node list.

### Node list (home)

The default screen — an at-a-glance overview of the mesh.

```
 42 nodes                              78%     <- header: node count + our battery
 ----------------------------------------
 * SPb Gate        ▂▄▆█   →0   now   RTR       <- one node per row
   ksv-relay       ▂▄▆_   →1   4m    CLI
   Neva Bridge     ▁___   →3   12m   RTR
 type a name to find a contact                 <- footer hint
```

- **Header:** how many nodes the node DB knows, and *our own* battery (or `USB` when powered).
- **Rows:** the top nodes in sorted order (see below). The home list is a fixed overview and
  does not scroll — to reach everyone else, open the picker.
- **Sort order:** favourites first → nodes you have a conversation with → then everyone else
  by hop distance (nearest first). *(The "conversation" tier is a placeholder until direct
  messages land.)*

Press any key, an arrow, or **ESC** to open the contact picker.

### Contact picker

Where you find and open a specific node. Opens from the home list on **ESC** or as soon as you
start **typing**.

```
 > ksv_                                        <- your query (type to filter by name)
 ----------------------------------------
   ksv-relay       ▂▄▆_   →1   4m    CLI       <- highlighted = current selection
   ksv-2           ▁___   →2   1h    CLI
 up/dn move  ENTER open  ESC back
```

- **Type** letters to filter the list by node name (case-insensitive).
- **↑ / ↓** move the selection; the list scrolls to keep it in view.
- **Enter** opens the highlighted node.
- **Backspace** deletes a query character.
- **ESC** returns to the home list.

Rows look identical to the home list, so nothing "changes shape" as you move around.

### Node view

Per-node detail (this is where a conversation will live once messaging is added). Shows the
node number, SNR, hops, and how long ago it was last heard. **ESC** or **Backspace** goes back
to the picker.

## Keys

| Key            | Home list        | Picker                    | Node view      |
| -------------- | ---------------- | ------------------------- | -------------- |
| letters        | open picker + filter | filter by name        | —              |
| ↑ / ↓          | open picker      | move selection            | —              |
| Enter          | open picker      | open selected node        | —              |
| Backspace      | open picker      | delete query character    | back to picker |
| ESC            | open picker      | back to home list         | back to picker |

## Reading a row

| Field        | Meaning                                                                        |
| ------------ | ------------------------------------------------------------------------------ |
| **name**     | node long name (falls back to short name, then `!nodenum`)                      |
| name colour  | white = normal · **orange `*`** = favourite · **yellow** = this node (us)       |
| **signal**   | 0–4 bars from the last direct SNR (green strong → orange weak)                  |
| empty bars   | no direct signal — the node is only reachable via relays                       |
| **`→N`**     | hops away: `→0` = direct neighbour, `→3` = via 3 relays, `→?` = unknown         |
| **age**      | last heard: `now` · `5m` · `2h` · `3d`; green when fresh, grey when stale       |
| age `?`      | never heard directly, or the clock isn't set                                   |
| **role**     | `CLI` client · `RTR` router · `RPT` repeater · `TRK` tracker · `SEN` sensor · `TAK` |

## Current limitations

- **No messaging yet.** The node view shows *(no messages yet)*; composing and sending text
  is the next step.
- **No per-node battery.** This build's compact node DB doesn't store other nodes' telemetry,
  so only *our* battery is shown (in the header). Other nodes show role instead.
- The home list is a top-N overview; use the picker to browse the full mesh.
