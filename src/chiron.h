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
    int  port;              // chiron-bridge port on the host
    char host[64];
    int  timeout_ms;        // give up and use vanilla text after this
    int  max_tokens;
    int  cache_size;        // generated blocks kept per session
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

// The file chiron_rewrite_block() writes the replacement block into.
#define CH_GEN_FILE "chiron_gen.txt"

// Engine address that asked for the current lookup; set by the text_open hook.
extern void* chiron_last_caller;
