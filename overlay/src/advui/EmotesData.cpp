// The stock emote artwork (graphics::emotes[] + its XBM bitmaps) is compiled out of
// our build: MESHTASTIC_EXCLUDE_SCREEN forces HAS_SCREEN 0, and emotes.cpp is gated on
// HAS_SCREEN. We run our own UI (no graphics::Screen / EmoteRenderer) but still want the
// UTF-8 -> bitmap table to render and pick emoji. So force just the DATA to compile here.
// Including the .cpp is deliberate: it has no Screen dependency (only configuration.h +
// emotes.h), and the normally-compiled emotes.cpp expands to nothing under HAS_SCREEN 0,
// so there is no duplicate definition.
#include "configuration.h"
#undef HAS_SCREEN
#define HAS_SCREEN 1
#ifdef EXCLUDE_EMOJI
#undef EXCLUDE_EMOJI
#endif
#include "graphics/emotes.cpp"
