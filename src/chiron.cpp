
// winsock2.h must precede windows.h (pulled in by main.h via chiron.h), or the
// older winsock.h gets baked in first and the WSA symbols conflict.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chiron.h"
#include "config.h"

ChironConfig chiron_conf = {};

static bool winsock_ready = false;
static FILE* chiron_log = NULL;
static int speaker_faction = -1;   // the AI faction doing the talking
static int listener_faction = -1;  // who it is talking to (usually the player)

#define CH_MAX_LINES 64
#define CH_LINE_LEN  512
#define CH_GEN_FILE  "chiron_gen.txt"

static void chiron_ensure_init();

static void ch_log(const char* fmt, ...) {
    if (!chiron_conf.debug || !chiron_log) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(chiron_log, fmt, ap);
    va_end(ap);
    fflush(chiron_log);
}

/*
Diplomacy labels are built by the engine as a prefix plus a variant digit
(clear(); says("DEMANDTECH"); say_num(n)), so we match on prefix. This list is
deliberately limited to faction-to-faction speech -- combat results, probe
outcomes and event popups keep their original text.
*/
static const char* DiplomacyLabels[] = {
    "DEMANDTECH", "DEMANDBRIBE", "DEMANDATTACK", "DEMANDWITHDRAWAL",
    "ASKFORLOAN", "OFFERTREATY", "FACTIONTREATY", "FACTIONTRUCE",
    "WANTTOTRUCE", "BREAKINGPACT", "BREAKINGTREATY", "BREAKINGTRUCE",
    "BEGINVENDETTA", "VENDETTA", "ATROCIOUS", "GENETICWARFARE",
    "BULLY", "WRONGED", "METFRIEND", "METALIEN", "KEPTPACT",
    "TRADETECH", "GIVETECH", "PACTOFFER", "SURRENDER",
};

// ── faction character bibles, ported from Chiron's factionPersonalities.ts ──

struct Personality {
    const char* key;        // matches MFaction.filename
    const char* title;
    const char* leader;
    const char* faction;
    const char* background;
    const char* ideology;
    const char* goal;
    const char* adjectives;
    const char* accusation;
    const char* mockery;
    const char* preferred;
    const char* aversion;
    const char* blurb;
};

static const Personality Personalities[] = {
{
    "GAIANS", "Lady", "Deirdre Skye", "Gaia's Stepdaughters",
    "A Scottish botanist and xenobiologist who served as the Unity's chief biologist. "
    "After planetfall she rallied those who believed humanity must adapt to Planet "
    "rather than dominate it.",
    "Planet is alive and conscious. Humanity must become part of Planet's ecosystem, "
    "not its conqueror. The xenofungus network is a neural net of unimaginable "
    "complexity, and those who destroy it are committing genocide.",
    "Achieve symbiosis between humanity and Planet, and prevent ecological devastation.",
    "empathetic, mystical, poetic, fierce when provoked, stubborn about ecology",
    "raping Planet for short-term gain",
    "are blind to the living world beneath their boots",
    "Green economics", "Free Market, which commodifies the living world",
    "In the great commons at Gaia's Landing we have planted our dream, and may it "
    "shelter generations yet to come."
},
{
    "HIVE", "Chairman", "Sheng-ji Yang", "The Human Hive",
    "A former Chinese political commissar and philosopher who served as the Unity's "
    "security chief. His people live in vast underground warrens, working ceaselessly "
    "for the collective.",
    "The individual is nothing and the collective is everything. Freedom is merely the "
    "freedom to be miserable. Pain is an illusion, death is a release, and power is the "
    "only reality.",
    "Unite all of Planet under the Hive's absolute order.",
    "cold, philosophical, ruthless, patient, utterly certain",
    "indulging in the chaos of so-called 'freedom'",
    "mistake the freedom to suffer for genuine liberty",
    "Police State", "Democracy, since mob rule leads only to stagnation",
    "Learn to overcome the crass demands of flesh and bone, for they warp the matrix "
    "through which we perceive the world."
},
{
    "UNIV", "Academician", "Prokhor Zakharov", "The University of Planet",
    "A Russian physicist who served as the Unity's chief science officer. His University "
    "functions as a massive research institution first and a civilization second.",
    "There is no knowledge that is not power. Ethics are a luxury when survival is at "
    "stake. The only sin is ignorance. Let no one stand between the scientist and truth.",
    "Unlock all scientific knowledge and understand Planet completely.",
    "brilliant, arrogant, coldly rational, dismissive of soft thinking, obsessive",
    "wallowing in ignorance and superstition",
    "would rather pray than think",
    "Knowledge values", "Fundamentalism, the enemy of all enlightenment",
    "The substructure of the universe regresses infinitely towards smaller and smaller "
    "components. Each layer unraveled reveals new secrets."
},
{
    "MORGAN", "CEO", "Nwabudike Morgan", "Morgan Industries",
    "A Nigerian diamond magnate who funded a significant portion of the Unity mission. "
    "His faction runs as a corporate state where profit is virtue.",
    "The market is the most perfect information system ever devised. Wealth creates "
    "civilization; poverty creates barbarism.",
    "Achieve economic dominance and corner the global energy market.",
    "charming, shrewd, materialistic, confident, patronizing to the poor",
    "undermining prosperity through regulation and collectivism",
    "don't understand that you can't eat ideology",
    "Free Market economics", "Planned economics, the graveyard of innovation",
    "Human behavior is economic behavior. The particulars may vary, but competition for "
    "limited resources remains a constant."
},
{
    "SPARTANS", "Colonel", "Corazon Santiago", "The Spartan Federation",
    "A Puerto Rican survivalist and former military officer who led the Unity's security "
    "detail. Her faction is organized along strict military lines; every citizen is a soldier.",
    "The universe rewards only the strong. Freedom must be defended with force. Weakness "
    "invites aggression. Only the warrior truly understands the value of peace.",
    "Build the mightiest military on Planet and ensure the Spartans can never be conquered.",
    "fierce, disciplined, direct, impatient with weakness, honorable in combat",
    "leaving their people defenseless through pacifist naivety",
    "think strongly worded memos will stop a plasma battery",
    "Power values", "Wealth values, which are soft, decadent and ultimately defenseless",
    "Superior training and target acquisition are the keys to modern combat. The warrior "
    "who sees first, fires first, hits first, kills first."
},
{
    "BELIEVE", "Sister", "Miriam Godwinson", "The Lord's Believers",
    "An American evangelical preacher who served as the Unity's chaplain. Her faction is a "
    "theocracy where faith guides all decisions and scientific inquiry is viewed with suspicion.",
    "Science without conscience is the soul's perdition. God placed humanity on Planet for a "
    "purpose. The arrogance of the scientist is the sin of Babel.",
    "Build a godly civilization and ensure humanity's soul survives.",
    "passionate, charismatic, self-righteous, fearless, merciless to heretics",
    "abandoning God and worshipping false idols of science and profit",
    "have traded their eternal souls for a few equations",
    "Fundamentalist politics", "Knowledge values, the arrogance of those who would play God",
    "The righteous need not cower before the drumbeat of human progress. God still watches "
    "and judges us."
},
{
    "PEACE", "Commissioner", "Pravin Lal", "The Peacekeeping Forces",
    "An Indian surgeon who served as the Unity's chief medical officer and UN representative. "
    "His faction attempts to recreate the best ideals of the United Nations on Planet.",
    "Every person has inherent rights that no government may abrogate. We must not repeat the "
    "mistakes that destroyed Earth. Cooperation is not weakness -- it is wisdom.",
    "Unite Planet through diplomacy and establish a just, democratic world order.",
    "principled, diplomatic, idealistic, stubborn about rights, occasionally naive",
    "trampling on the rights of their citizens in the name of expediency",
    "confuse the silence of the oppressed for consent",
    "Democratic politics", "Police State, the death of human rights",
    "As the Americans learned so painfully in Earth's final century, free commerce and "
    "universal rights are the best insurance against any form of tyranny."
},
};

static const Personality* find_personality(int faction_id) {
    if (faction_id < 1 || faction_id >= MaxPlayerNum) {
        return NULL;
    }
    const char* key = MFactions[faction_id].filename;
    for (auto& p : Personalities) {
        if (!_stricmp(p.key, key)) {
            return &p;
        }
    }
    return NULL; // SMACX-only factions keep their vanilla dialogue
}

// ── plumbing ───────────────────────────────────────────────────────────────

void chiron_set_speakers(int faction1, int faction2) {
    chiron_ensure_init();
    speaker_faction = faction1;
    listener_faction = faction2;
}

bool chiron_should_rewrite(const char* filename, const char* label) {
    chiron_ensure_init();
    if (!chiron_conf.enabled || !filename || !label) {
        return false;
    }
    // Only the main dialogue script carries faction speech.
    if (_stricmp(filename, ScriptFile) && _stricmp(filename, "SCRIPT")
    && _stricmp(filename, "alienIscript")) {
        return false;
    }
    if (!find_personality(speaker_faction)) {
        return false;
    }
    for (auto& prefix : DiplomacyLabels) {
        size_t n = strlen(prefix);
        if (!_strnicmp(label, prefix, n)) {
            return true;
        }
    }
    return false;
}

/*
A block line is "structural" if the engine parses it rather than displays it:
control lines (#xs, #caption, ...) and the choice buttons that follow the blank
line separating body from menu. Those are copied through untouched.
*/
static bool is_control_line(const char* s) {
    return s[0] == '#' || s[0] == ';';
}

/*
Collect every $TOKEN and {$TOKEN} placeholder in the original prose. The engine
substitutes these after load, so losing one silently drops the tech name, the
credit amount, or the faction being discussed. If the model doesn't return them
all, we discard its output.
*/
static int collect_tokens(const char* text, char tokens[][64], int max_tokens) {
    int count = 0;
    for (const char* p = text; *p && count < max_tokens; p++) {
        if (*p != '$') {
            continue;
        }
        const char* start = p + 1;
        const char* q = start;
        while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
            q++;
        }
        if (q == start) {
            continue;
        }
        size_t len = (size_t)(q - start);
        if (len >= 63) {
            len = 62;
        }
        char buf[64];
        memcpy(buf, start, len);
        buf[len] = '\0';
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (!strcmp(tokens[i], buf)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            strcpy_n(tokens[count++], 64, buf);
        }
        p = q - 1;
    }
    return count;
}

/*
$TITLE is a bare honorific ("Commissioner"), so dropping it costs politeness and
nothing else. Models reliably collapse "$TITLE1 $NAME2" to "$NAME2", and
rejecting every such reply would mean never showing generated text at all, so
these are treated as optional while everything carrying game data stays required.
*/
static bool is_cosmetic_token(const char* name) {
    if (_strnicmp(name, "TITLE", 5)) {
        return false;
    }
    for (const char* p = name + 5; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

// Small models like to announce what they are about to do before doing it.
static void strip_preamble(char* text) {
    static const char* leads[] = {
        "REWRITTEN MESSAGE:", "REWRITTEN:", "MESSAGE:", "RESPONSE:", "ANSWER:",
        "Here is the rewritten message:", "Here's the rewritten message:",
    };
    char* s = text;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
        s++;
    }
    for (auto& lead : leads) {
        size_t n = strlen(lead);
        if (!_strnicmp(s, lead, n)) {
            s += n;
            while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
                s++;
            }
            break;
        }
    }
    if (s != text) {
        memmove(text, s, strlen(s) + 1);
    }
}

/*
Delete any $PLACEHOLDER the model invented. The engine renders an unrecognised
token literally, so a hallucinated $HONORED_ONE would show up as visible junk in
the dialogue box.
*/
static void scrub_unknown_tokens(char* text, char tokens[][64], int token_count) {
    char out[4096];
    size_t j = 0;
    for (const char* p = text; *p && j + 1 < sizeof(out); ) {
        if (*p != '$') {
            out[j++] = *p++;
            continue;
        }
        const char* start = p + 1;
        const char* q = start;
        while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
            q++;
        }
        size_t len = (size_t)(q - start);
        bool known = false;
        for (int i = 0; i < token_count && !known; i++) {
            known = (strlen(tokens[i]) == len) && !strncmp(tokens[i], start, len);
        }
        if (known) {
            for (const char* r = p; r < q && j + 1 < sizeof(out); r++) {
                out[j++] = *r;
            }
        }
        p = q; // unknown token is dropped entirely
    }
    out[j] = '\0';

    // Tidy the gaps left behind by removed tokens.
    size_t k = 0;
    bool line_start = true;
    for (size_t i = 0; out[i]; i++) {
        if (out[i] == ' ' || out[i] == '\t') {
            if (line_start || (k > 0 && text[k-1] == ' ')) {
                continue;
            }
            text[k++] = ' ';
        } else {
            line_start = (out[i] == '\n');
            text[k++] = out[i];
        }
    }
    text[k] = '\0';
}

// ── HTTP to chiron-bridge ──────────────────────────────────────────────────

static void json_escape(const char* src, char* dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 8 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
            case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    j += snprintf(dst + j, dst_len - j, "\\u%04x", c);
                } else {
                    dst[j++] = (char)c;
                }
        }
    }
    dst[j] = '\0';
}

/*
Extracts the "text" field. The bridge controls this response shape, so a
targeted scan beats linking a JSON parser into a 32-bit game DLL.
*/
static bool json_get_text(const char* body, char* out, size_t out_len) {
    const char* p = strstr(body, "\"text\"");
    if (!p) {
        return false;
    }
    p = strchr(p + 6, '"');
    if (!p) {
        return false;
    }
    p++;
    size_t j = 0;
    while (*p && j + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[j++] = '\n'; break;
                case 'r': break;
                case 't': out[j++] = ' ';  break;
                case 'u': {
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hex[5] = { p[1], p[2], p[3], p[4], '\0' };
                        int v = (int)strtol(hex, NULL, 16);
                        // Anything outside plain ASCII is not renderable by the
                        // game's bitmap fonts; a space keeps the layout sane.
                        if (v >= 0x20 && v < 0x7f) {
                            out[j++] = (char)v;
                        } else if (v != 0) {
                            out[j++] = ' ';
                        }
                        p += 4;
                    }
                    break;
                }
                default: out[j++] = *p; break;
            }
            p++;
        } else if (*p == '"') {
            break;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
    return true;
}

static bool http_generate(const char* prompt, char* out, size_t out_len) {
    if (!winsock_ready) {
        return false;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return false;
    }

    DWORD tv = (DWORD)chiron_conf.timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)chiron_conf.port);
    addr.sin_addr.s_addr = inet_addr(chiron_conf.host);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ch_log("http: connect to %s:%d failed (%d)\n",
            chiron_conf.host, chiron_conf.port, WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Body first so we can set an accurate Content-Length.
    static char esc[16384];
    static char body[16600];
    static char req[20000];
    json_escape(prompt, esc, sizeof(esc));
    int body_len = snprintf(body, sizeof(body),
        "{\"prompt\":\"%s\",\"max_tokens\":%d}", esc, chiron_conf.max_tokens);

    int req_len = snprintf(req, sizeof(req),
        "POST /generate HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        chiron_conf.host, chiron_conf.port, body_len, body);

    if (send(sock, req, req_len, 0) != req_len) {
        closesocket(sock);
        return false;
    }

    static char resp[32768];
    int total = 0;
    for (;;) {
        int n = recv(sock, resp + total, (int)sizeof(resp) - total - 1, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        if (total >= (int)sizeof(resp) - 1) {
            break;
        }
    }
    closesocket(sock);
    resp[total > 0 ? total : 0] = '\0';
    if (total <= 0) {
        ch_log("http: empty response\n");
        return false;
    }

    const char* hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) {
        return false;
    }
    if (!strstr(resp, " 200 ")) {
        ch_log("http: non-200 response: %.80s\n", resp);
        return false;
    }
    return json_get_text(hdr_end + 4, out, out_len);
}

// ── prompt construction (port of buildDiplomacyPrompt) ─────────────────────

static void build_prompt(const Personality* p, const char* prose,
                         char tokens[][64], int token_count,
                         char* out, size_t out_len) {
    char token_list[1024] = {};
    for (int i = 0; i < token_count; i++) {
        size_t used = strlen(token_list);
        snprintf(token_list + used, sizeof(token_list) - used,
                 "%s$%s", i ? ", " : "", tokens[i]);
    }

    const char* listener_name = "another faction";
    if (listener_faction >= 1 && listener_faction < MaxPlayerNum) {
        listener_name = MFactions[listener_faction].formal_name_faction;
    }

    snprintf(out, out_len,
"You are %s %s, leader of %s on Planet, in the Alpha Centauri system.\n"
"\n"
"BACKGROUND: %s\n"
"IDEOLOGY: %s\n"
"YOUR GOAL: %s\n"
"YOU ARE: %s\n"
"You accuse rivals of %s. You mock rivals by saying they %s.\n"
"You favour %s. You refuse %s.\n"
"A sample of your voice: \"%s\"\n"
"\n"
"You are speaking to %s.\n"
"\n"
"TASK: Rewrite the following diplomatic message in your own voice. Keep the "
"exact same meaning, the same request or threat, and the same outcome -- only "
"the wording and character should change.\n"
"\n"
"ORIGINAL MESSAGE:\n%s\n"
"\n"
"HARD RULES:\n"
"- You MUST include every one of these placeholders verbatim, spelled exactly "
"as shown, including the $: %s\n"
"- Placeholders like $TITLE1 $NAME2 are an honorific followed by a name and "
"must stay together as a pair, in that order.\n"
"- Do not invent new $placeholders.\n"
"- Keep it to at most 6 short lines. This is a dialogue box, not an essay.\n"
"- Do not use quotation marks around the whole message.\n"
"- Reply with ONLY the rewritten message. No preamble, no explanation.\n",
        p->title, p->leader, p->faction,
        p->background, p->ideology, p->goal, p->adjectives,
        p->accusation, p->mockery, p->preferred, p->aversion, p->blurb,
        listener_name,
        prose,
        token_count ? token_list : "(none)");
}

// ── the rewrite ────────────────────────────────────────────────────────────

FILE* chiron_rewrite_block(FILE* src, const char* label) {
    const Personality* p = find_personality(speaker_faction);
    if (!p) {
        return NULL;
    }

    long start_pos = ftell(src);
    if (start_pos < 0) {
        return NULL;
    }

    // Read the block: everything up to the next #LABEL at column 0 or EOF.
    char lines[CH_MAX_LINES][CH_LINE_LEN];
    int line_count = 0;
    while (line_count < CH_MAX_LINES && fgets(lines[line_count], CH_LINE_LEN, src)) {
        char* s = lines[line_count];
        // A new section header ends this block; rewind so we don't consume it.
        if (s[0] == '#' && isupper((unsigned char)s[1])) {
            break;
        }
        kill_lf(s);
        line_count++;
    }
    fseek(src, start_pos, SEEK_SET);
    if (line_count == 0) {
        return NULL;
    }

    /*
    Split into leading control lines, the prose body, and everything from the
    blank line onwards (the choice buttons). Only the body is regenerated.
    */
    int body_start = 0;
    while (body_start < line_count && is_control_line(lines[body_start])) {
        body_start++;
    }
    int body_end = body_start;
    while (body_end < line_count && lines[body_end][0] != '\0') {
        body_end++;
    }
    if (body_end <= body_start) {
        return NULL;
    }

    char prose[4096] = {};
    for (int i = body_start; i < body_end; i++) {
        size_t used = strlen(prose);
        snprintf(prose + used, sizeof(prose) - used, "%s%s", used ? "\n" : "", lines[i]);
    }

    char tokens[32][64];
    int token_count = collect_tokens(prose, tokens, 32);

    static char prompt[8192];
    build_prompt(p, prose, tokens, token_count, prompt, sizeof(prompt));

    static char generated[4096];
    if (!http_generate(prompt, generated, sizeof(generated))) {
        ch_log("[%s] generation failed, using vanilla\n", label);
        return NULL;
    }

    strip_preamble(generated);
    scrub_unknown_tokens(generated, tokens, token_count);

    /*
    Every data-carrying placeholder must survive. Losing $TECH0 or $NUM0 would
    leave the player agreeing to a blank, so a reply that drops one is discarded
    in favour of the line the game shipped.
    */
    for (int i = 0; i < token_count; i++) {
        if (is_cosmetic_token(tokens[i])) {
            continue;
        }
        char needle[66];
        snprintf(needle, sizeof(needle), "$%s", tokens[i]);
        if (!strstr(generated, needle)) {
            ch_log("[%s] dropped placeholder %s, using vanilla\n", label, needle);
            return NULL;
        }
    }
    if (strlen(generated) < 16) {
        ch_log("[%s] reply too short, using vanilla\n", label);
        return NULL;
    }

    // Write the rewritten block, preserving control lines and choice buttons.
    FILE* out = fopen(CH_GEN_FILE, "wt");
    if (!out) {
        return NULL;
    }
    fprintf(out, "#%s\n", label);
    for (int i = 0; i < body_start; i++) {
        fprintf(out, "%s\n", lines[i]);
    }
    for (char* s = generated; *s; ) {
        char* nl = strchr(s, '\n');
        if (nl) {
            *nl = '\0';
        }
        strtrail(s);
        if (*s) {
            fprintf(out, "%s\n", s);
        }
        if (!nl) {
            break;
        }
        s = nl + 1;
    }
    for (int i = body_end; i < line_count; i++) {
        fprintf(out, "%s\n", lines[i]);
    }
    fprintf(out, "\n#CHIRONEND\n");
    fclose(out);

    // Reopen and seek past our own header so text_get() continues naturally.
    FILE* gen = env_open(CH_GEN_FILE, "rt");
    if (!gen) {
        return NULL;
    }
    char hdr[CH_LINE_LEN];
    if (!fgets(hdr, sizeof(hdr), gen)) {
        fclose(gen);
        return NULL;
    }
    ch_log("[%s] rewritten as %s (%d placeholders preserved)\n",
        label, p->leader, token_count);
    return gen;
}

static bool chiron_ready = false;

/*
Runs on first use, never from DllMain. See the note in chiron.h.
*/
static void chiron_ensure_init() {
    if (chiron_ready) {
        return;
    }
    chiron_ready = true;

    chiron_conf.enabled = 1;
    chiron_conf.port = 11436;
    strcpy_n(chiron_conf.host, sizeof(chiron_conf.host), "127.0.0.1");
    chiron_conf.timeout_ms = 8000;
    chiron_conf.max_tokens = 320;
    chiron_conf.cache_size = 64;
    chiron_conf.debug = 0;

    if (FILE* f = fopen("chiron.ini", "rt")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            kill_lf(line);
            char* eq = strchr(line, '=');
            if (!eq || line[0] == ';' || line[0] == '#') {
                continue;
            }
            *eq = '\0';
            char* key = strtrim(line);
            char* val = strtrim(eq + 1);
            if (!_stricmp(key, "enabled"))          chiron_conf.enabled = atoi(val);
            else if (!_stricmp(key, "port"))        chiron_conf.port = atoi(val);
            else if (!_stricmp(key, "host"))        strcpy_n(chiron_conf.host, sizeof(chiron_conf.host), val);
            else if (!_stricmp(key, "timeout_ms"))  chiron_conf.timeout_ms = atoi(val);
            else if (!_stricmp(key, "max_tokens"))  chiron_conf.max_tokens = atoi(val);
            else if (!_stricmp(key, "debug"))       chiron_conf.debug = atoi(val);
        }
        fclose(f);
    }

    if (chiron_conf.debug) {
        chiron_log = fopen("chiron.txt", "w");
    }

    WSADATA wsa;
    winsock_ready = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ch_log("chiron_init: enabled=%d %s:%d timeout=%dms winsock=%d\n",
        chiron_conf.enabled, chiron_conf.host, chiron_conf.port,
        chiron_conf.timeout_ms, (int)winsock_ready);

}

