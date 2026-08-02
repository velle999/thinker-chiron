#pragma once

#include "main.h"

/*
Chiron Rising — LLM-driven diplomacy for SMACX.

Ported from velle999/Chiron-Rising's LLM layer (src/llm). The game's canned
diplomacy lines are replaced at read time with text generated in character by a
local model, reached over HTTP through chiron-bridge (which fronts synapd).

Interception happens in text_open(): once the engine has seeked to a #LABEL,
we read the vanilla block, ask the model to rewrite its prose, and swap the
FILE* for one holding the rewritten block. text_get() is unchanged -- it keeps
fgets()ing lines, unaware the file underneath it is ours.

Every failure path falls back to the vanilla block. A dropped $TOKEN, a timeout,
a dead bridge, an empty reply: all of them mean "show what the game shipped".
*/

struct ChironConfig {
    int  enabled;           // master switch
    int  port;              // 0 until resolved from the backend's default
    char host[64];
    char backend[16];       // bridge (default), ollama, or llamacpp
    char model[64];         // ollama only: which model to ask for
    int  timeout_ms;        // give up and use vanilla text after this
    int  max_tokens;
    int  cache_size;        // generated blocks kept per session
    int  base_names;        // name new bases from the faction's culture
    int  probe_protests;    // let the player object to probe operations
    int  debug;             // write chiron.txt log
};

extern ChironConfig chiron_conf;

/*
There is deliberately no init entry point called from DllMain.

Setup runs lazily on first use instead. Winsock startup loads further DLLs, and
doing that from DllMain means requesting the loader lock while already holding
it -- which left the game unable to allocate its draw buffer, long after this
code had finished. Everything below is safe to call from normal game context.
*/

/*
Unconditional breadcrumb log, written to chiron_trace.txt in the game folder.

Separate from the chiron.txt debug log on purpose. chiron.txt only appears once
chiron.ini has been found and parsed and debug=1 read out of it, so its absence
is ambiguous -- it cannot distinguish "the DLL never loaded" from "the game died
before the first text lookup" from "the config was not found". This one is plain
fopen/fputs/fclose with no configuration behind it, so the last line it contains
is exactly how far the process got.

Safe to call from DllMain: it touches only msvcrt, which is already resident as a
static import, and never loads a DLL. Delete the file to reset it.
*/
void chiron_trace(const char* fmt, ...);

// Records who is speaking to whom; called from mod_diplomacy_caption.
void chiron_set_speakers(int faction1, int faction2);

// True when this filename/label pair is diplomacy dialogue we should rewrite.
bool chiron_should_rewrite(const char* filename, const char* label);

/*
Reads the remainder of the block from src (positioned just after the #LABEL
header), generates a replacement, and returns a FILE* positioned just after the
header of the rewritten block. Returns NULL to mean "use the vanilla block" --
src is left untouched and still usable in that case.
*/
FILE* chiron_rewrite_block(FILE* src, const char* label);

/*
Name a newly founded base from the faction's culture. Writes at most
MaxBaseNameLen bytes into name and returns true; false means "use the name the
engine would have picked", which is what happens for factions with no character
bible, a dead bridge, or an unusable reply.

Called from mod_name_base before any of Thinker's own list handling, so a false
return leaves the vanilla path exactly as it was, offsets and all.
*/
bool chiron_name_base(int faction_id, char* name, bool sea_base);

/*
Generate and show the Planetnet dispatch: an in-fiction summary of the state of
Planet, built from engine state. Bound to Alt+N in the map window.

The only feature here with no vanilla text behind it, so it is the only one the
player invokes deliberately -- a couple of seconds is a wait you asked for,
where the same pause during turn processing would read as the game hanging.
Renders through #CHIRONNEWS in modmenu.txt, which must therefore be the copy
install.sh ships.
*/
void chiron_show_news();

/*
The mod's own menu, bound to Alt+M in the map window.

Answers the one question the design cannot otherwise answer from the seat: is
this working? Every failure here falls back to the game's own text, so a dead
backend and an uninstalled mod look identical on screen. The menu says which,
carries a connection test, and lets the switches be thrown for the session
without editing chiron.ini and restarting.

Renders through #CHIRONMENU in modmenu.txt -- another reason that file must be
the copy install.sh ships.
*/
void chiron_show_menu();

/*
True while faction_id has agreed to a warning from tgt_faction and the reprieve
has not lapsed. Read by the probe gate in veh_action.cpp, which is what makes an
agreement mean anything.
*/
bool chiron_probe_warned(int faction_id, int tgt_faction);

/*
True if faction_id refused a warning from tgt_faction recently enough for it
still to be grounds for war. Read by double_cross() in faction.cpp, where it
feeds the engine's own is_victim flag so that breaking with them costs no
integrity -- the same treatment the game already gives an atrocity victim.

Note the direction, which an earlier comment here had backwards: the refusal is
stored on the faction that REFUSED, indexed by whoever warned them. So this asks
"did faction_id refuse tgt_faction", and the call site passes the defender first
because it is the defender's refusal that excuses the attacker.
*/
bool chiron_probe_refused(int faction_id, int tgt_faction);

/*
Per-turn check for probe operations in either direction, opening the
confrontation when one is found. Called from faction_upkeep.
*/
void chiron_check_thefts(int faction_id);

/*
The player is under a warning they gave and is about to probe anyway. Asks
whether they mean it; on yes the promise is discharged and the engine's own
double-cross counters record it. Read by the probe gate in veh_action.cpp.
*/
bool chiron_confirm_break_word(int breaker, int tgt);

/*
Is there an unresolved probe grievance the player could raise with this faction?
True while they have robbed us and the matter has been neither conceded nor
refused within the warning window.
*/
bool chiron_probe_grievance(int player_id, int ai_id);

/*
Offer to raise it at the top of a diplomacy conversation, so the confrontation
is something the player can start rather than only something they are handed at
turn start. Called from mod_diplomacy_menu.
*/
void chiron_offer_raise(int player_id, int ai_id);

// The file chiron_rewrite_block() writes the replacement block into.
#define CH_GEN_FILE "chiron_gen.txt"

// Engine address that asked for the current lookup; set by the text_open hook.
extern void* chiron_last_caller;
