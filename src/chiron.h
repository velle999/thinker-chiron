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
