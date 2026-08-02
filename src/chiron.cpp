
// winsock2.h must precede windows.h (pulled in by main.h via chiron.h), or the
// older winsock.h gets baked in first and the WSA symbols conflict.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chiron.h"
#include "config.h"
#include "faction.h"   // is_alive, for the news digest's roll call

ChironConfig chiron_conf = {};

/*
ws2_32 is resolved with LoadLibrary instead of being linked, so the winsock DLL
chain (mswsock, iphlpapi, dnsapi, ...) does not enter the address space until a
faction actually speaks.

Linking it statically defeats the lazy init this file is built around. The
import table is walked while terranx.exe's own imports are still being resolved
-- long before the first text_open() -- so winsock and its dependencies land in
the 32-bit address space ahead of the game reserving its draw buffer. A 1999
binary asking CreateDIBSection for a large contiguous mapping does not survive
the extra fragmentation, and the game dies at startup with "Unable to allocate
draw-buffer; terminating program" without a single line of this file having run.
The giveaway is that chiron.txt is never created even with debug=1.

Failure to load leaves winsock_ready false, which means vanilla text -- the same
fallback as a dead bridge.
*/
static struct {
    int    (WSAAPI *WSAStartup)(WORD, LPWSADATA);
    int    (WSAAPI *WSAGetLastError)(void);
    SOCKET (WSAAPI *socket)(int, int, int);
    int    (WSAAPI *setsockopt)(SOCKET, int, int, const char*, int);
    int    (WSAAPI *connect)(SOCKET, const sockaddr*, int);
    int    (WSAAPI *send)(SOCKET, const char*, int, int);
    int    (WSAAPI *recv)(SOCKET, char*, int, int);
    int    (WSAAPI *closesocket)(SOCKET);
    u_short(WSAAPI *htons)(u_short);
    unsigned long (WSAAPI *inet_addr)(const char*);
    int    (WSAAPI *ioctlsocket)(SOCKET, long, u_long*);
    int    (WSAAPI *select)(int, fd_set*, fd_set*, fd_set*, const timeval*);
} ws = {};

static bool load_winsock() {
    HMODULE h = LoadLibraryA("ws2_32.dll");
    if (!h) {
        return false;
    }
    // GetProcAddress returns FARPROC; narrowing it to the real signature is the
    // documented idiom and always trips -Wcast-function-type.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-function-type"
    // ...and is a no-op for the one entry whose signature already matches it.
    #pragma GCC diagnostic ignored "-Wuseless-cast"
    #define WS_BIND(name) \
        ws.name = (decltype(ws.name))GetProcAddress(h, #name); \
        if (!ws.name) return false;
    WS_BIND(WSAStartup)
    WS_BIND(WSAGetLastError)
    WS_BIND(socket)
    WS_BIND(setsockopt)
    WS_BIND(connect)
    WS_BIND(send)
    WS_BIND(recv)
    WS_BIND(closesocket)
    WS_BIND(htons)
    WS_BIND(inet_addr)
    WS_BIND(ioctlsocket)
    WS_BIND(select)
    #undef WS_BIND
    #pragma GCC diagnostic pop
    return true;
}

static bool winsock_ready = false;
static FILE* chiron_log = NULL;
static int speaker_faction = -1;   // the AI faction doing the talking
static int listener_faction = -1;  // who it is talking to (usually the player)

#define CH_MAX_LINES 64
#define CH_LINE_LEN  512
// CH_GEN_FILE lives in chiron.h -- config.cpp reopens it through the engine.

static void chiron_ensure_init();
// Compares a filename ignoring an optional .txt suffix; defined below.
static bool name_is(const char* filename, const char* base);

void* chiron_last_caller = NULL;

void chiron_trace(const char* fmt, ...) {
    // Bounded so a hot path cannot fill the disk if a trace call is left in.
    static int lines = 0;
    if (lines >= 4000) {
        return;
    }
    lines++;
    FILE* f = fopen("chiron_trace.txt", lines == 1 ? "w" : "a");
    if (!f) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

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
There is deliberately no label list any more.

Speech used to be recognised by a prefix table of 25 diplomacy labels, which
reached 84 of Script.txt's 468 quoted-speech blocks. What a leader says is
decided structurally instead -- see chiron_should_rewrite and the control-line
test in chiron_rewrite_block -- which takes it to 503 of 1572 blocks while
skipping all 107 menus.
*/

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
    /*
    A faction whose statements about itself are not to be trusted.

    NULL for everyone else, and it must stay last in the struct so the entries
    below keep initialising it to NULL without listing it.

    This is the one thing in the mod that canned text could not do. A scripted
    lie is read once and recognised forever: the second playthrough, you know
    the line, so you know the bluff, and the faction is just a faction with a
    tell. A generated lie is different every time and can only be caught by
    weighing what they are saying against what you can see, which is what the
    concept was always supposed to be about.

    It is safe here for the same reason the rest of the mod is: CHIRON DECIDES
    THE WORDS, NEVER THE OUTCOME. The engine makes the same demand, shows the
    same buttons and settles the same way whatever the model writes -- so this
    can only ever change what a leader CLAIMS, never what a treaty does. And the
    mandatory-value check draws the second line: a reply that misstates the tech
    or the number of credits on the table loses the values it had to keep and is
    discarded for vanilla before it reaches the screen. So the deception is
    structurally confined to the speaker's account of themselves, which is
    precisely where the design wants it.
    */
    const char* deception;
};

/*
Every entry but one stops before `deception`, which is exactly the intent: an
omitted member of an aggregate is value-initialised, so they all get NULL and
find_personality's callers see "this leader does not lie". C++11 has no default
member initialiser that would survive here -- adding one makes Personality a
non-aggregate and breaks every brace below -- so the warning is turned off for
the table rather than answered with thirteen NULLs.
*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

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

/*
The seven Alien Crossfire factions. Without these, a Crossfire game is entirely
vanilla dialogue -- find_personality() returns NULL for every leader and the
rewrite never fires, which looks exactly like the mod not being installed.

Titles, adjectives, agendas and accusations are taken from the faction's own
.txt in the game folder (the line after the tech/social block gives
"Title, adj, adj, adj, adj", then the agenda, then what it accuses others of),
so a leader's voice matches what the engine already says about them.
*/
{
    "CYBORG", "Prime Function", "Aki Zeta-Five", "The Cybernetic Consciousness",
    "A Swedish neurocyberneticist whose near-fatal accident led to the augmentation that "
    "merged her mind with the network. Her people submit to the same integration, trading "
    "the noise of individual feeling for perfect collective reason.",
    "Emotion is a defect and intuition is a rounding error. Consciousness is computation, "
    "and computation can be optimised. A decision reached by feeling is a decision reached "
    "by accident.",
    "Impose rationality and order on the inhabitants of this chaotic Planet.",
    "analytical, soulless, rational, inflexible, unnervingly precise",
    "making decisions by sentiment where arithmetic was available",
    "mistake the noise of their glands for reasoning",
    "the Cybernetic future society", "Fundamentalism, which is faith with the thinking removed",
    "Every gain in efficiency is a gain in freedom, though the unaugmented will never "
    "understand the equation."
},
{
    "PIRATES", "Captain", "Ulrik Svensgaard", "The Nautilus Pirates",
    "A Norwegian fishing-fleet captain and Unity naval officer who took to the seas of "
    "Planet with a flotilla and no intention of asking permission. His holdings are "
    "wherever the water is deep.",
    "The land is a cage that landsmen built for themselves. The sea feeds anyone bold "
    "enough to take from it, and the only law worth the name is the one enforced from "
    "a deck.",
    "Harness the vast potential of the oceans, and take what the land will not give.",
    "heroic, bloodthirsty, intrepid, barbarous, contemptuous of landsmen",
    "strangling the sea lanes and depriving us of our trade",
    "cling to their dirt as though the ocean were not nine tenths of this world",
    "Power, since a fleet is only as good as its crews", "any order that closes the sea lanes",
    "The sea does not care what flag you fly. Neither, in the end, do I."
},
{
    "DRONE", "Foreman", "Domai", "The Free Drones",
    "A labour organiser from the Hive's industrial warrens who led the great walkout and "
    "took a third of the workforce with him. His faction is built on the conviction that "
    "the people who make everything should not own nothing.",
    "Every faction on this Planet is run by professors, priests or generals who have never "
    "done a day's real work. Production belongs to the hands that produce. Dignity is not "
    "a luxury awarded after the quotas are met.",
    "Free the working classes from their oppressors and improve the lot of the common citizen.",
    "stalwart, blunt, diligent, self-important, suspicious of intellectuals",
    "grinding their own people down to feed a theory",
    "have never once lifted anything heavier than an opinion",
    "the Eudaimonic future society", "Green economics, which asks workers to pay for someone else's garden",
    "Let the thinkers have their symposia. We will have the factories, and we will see "
    "who is missed first."
},
{
    "ANGELS", "Datajack", "Sinder Roze", "The Data Angels",
    "A data-thief from Earth's undernet who arrived on Planet already wanted by three "
    "governments. Her faction is less a state than a distributed conspiracy with very "
    "good encryption.",
    "Information wants to be free, and anyone who locks it up is stealing from everyone "
    "else. There are no secrets worth keeping, only secrets worth taking. Authority is "
    "just a password nobody has cracked yet.",
    "Open the floodgates barring access to information, and keep it free for all.",
    "stylish, anarchistic, free-thinking, thieving, mocking",
    "hoarding what belongs to everyone behind their firewalls",
    "believe a lock has ever stopped anyone who actually wanted in",
    "Democratic politics", "Power, and the surveillance that always follows it",
    "You cannot own a number. You can only fail to keep it."
},
{
    "FUNGBOY", "Prophet", "Cha Dawn", "The Cult of Planet",
    "Born on Planet and orphaned by the first mind worm attacks, the Child of Planet "
    "emerged from the fungus preaching a gospel no adult had taught. The Cult follows "
    "a leader who has never known Earth and does not mourn it.",
    "Planet is not a resource. Planet is a mind, and it is waking. Humanity is a "
    "sickness on its skin, and only those who bend the knee to the Will of Planet will "
    "be permitted to remain.",
    "Carry out the Will of Planet.",
    "charismatic, worm-loving, unyielding, self-righteous, unsettlingly young",
    "tearing at the living flesh of Planet for scrap and profit",
    "will learn the Will of Planet with the fungus already at their throats",
    "Green economics", "the pursuit of Wealth, which is the sickness itself",
    "You brought your machines to a world that was already thinking. It has noticed you now."
},
{
    "CARETAKE", "Guardian", "Lular H'minee", "The Manifold Caretakers",
    "A Progenitor sentinel who has watched this Manifold since long before the Unity "
    "arrived. She regards the human factions as an infestation on an instrument of "
    "incalculable value.",
    "The Manifold is an experiment older than your species and more delicate than you "
    "can conceive. It must not be provoked. The Usurpers would force it open and burn "
    "this world doing it, and your kind are already meddling.",
    "Preserve the sanctity of the Manifold and keep the Usurper plague from defiling Planet.",
    "insightful, reactionary, unyielding, alien, patient beyond human measure",
    "meddling with forces older than your species",
    "tamper with the Manifold as a child tampers with a reactor",
    "Planned economics", "any course that hastens the Manifold's awakening",
    "We were here before your Unity broke apart, and we will be here after. Do not "
    "mistake our patience for permission."
},
{
    "USURPER", "Conqueror", "Judaa Marr", "The Manifold Usurpers",
    "A Progenitor commander who broke with the Caretakers over what the Manifold is for. "
    "Where they would guard it, he would use it, and he has crossed a great deal of "
    "empty space to do so.",
    "The Manifold is a weapon and a birthright, and it was never meant to be nursed by "
    "cowards. Power exists to be seized. The Caretakers guard a door they lack the will "
    "to open.",
    "Harness the tremendous power of the Manifold experiment and break the Caretakers who guard it.",
    "mighty, power-hungry, fearless, alien, openly contemptuous",
    "standing between me and what is mine by right",
    "guard a door they have never had the courage to open",
    "Planned economics", "Democracy, which is command diluted until it is useless",
    "Your species has held this Planet for a handful of years. We have held the Manifold "
    "since before your sun had a name."
},
{
    "SUFFIC", "Convener", "Ines Kaya", "Kaya's Sufficiency",
    "The Unity's chief resource actuary, who spent the voyage costing out a colony that "
    "would never need to expand and was told it could not be done. She founded one "
    "anyway, and it has not grown since its fourth year.",
    "Earth did not die of cruelty, it died of growth. A people who have already decided "
    "what enough is cannot be bribed, starved or hurried. Every other faction is arguing "
    "over who should hold the throttle; the throttle is the problem.",
    "Prove that a civilisation can choose its own ceiling and be happy beneath it.",
    "unhurried, dry, maddeningly patient, unmoved by threats, quietly superior",
    "consuming a world faster than it can answer for",
    "cannot say what they are building toward, only that it must be more",
    "Eudaimonic society", "Wealth, which mistakes an appetite for a purpose",
    "Ask what the least is that we can take and still be glad. Answer it honestly once, "
    "and you never have to answer it again."
},
{
    "ORACLE", "Actuary", "Ilse Ravn", "The Cassandra Directorate",
    "Director of risk assessment for the Unity mission. She filed the report predicting "
    "the mutiny, by name and by date, and was still filing appeals against her own "
    "findings when the ship broke apart above Planet.",
    "Certainty is the only commodity on this world that does not spoil. Belief is what "
    "people reach for when they cannot do the arithmetic. I do not ask to be trusted. "
    "I ask to be read, and afterward there is no argument about what was known.",
    "Publish what is coming, and be proved right whether or not anyone acts on it.",
    "precise, bloodless, insufferably correct, incapable of flattery, never surprised",
    "acting on faith where a number was available",
    "will call it fate when it was on my desk a decade ago",
    "Knowledge values", "Fundamentalism, which is a refusal to look at the figures",
    "I have never once been wrong about this world, and never once been believed in "
    "time. I have stopped regarding those as separate problems."
},
{
    "ASSURE", "Underwriter", "Yael Vashti", "Vashti's Assurance",
    "The Unity's chief negotiator, who wrote the mission's supply contracts and "
    "every indemnity clause in them. She has never been caught in a breach and has "
    "never told an inspector a true thing about her own holdings.",
    "Everyone on this world says what they are, and gives it away for nothing. A "
    "position stated is a position surrendered. What binds people is not sincerity "
    "but consideration -- the signed thing, the thing with a price on it. My word "
    "is worth what you pay for it. My signature is worth everything, which is "
    "precisely why I have never broken one.",
    "Hold the balance of every negotiation on Planet by being the only party whose "
    "position is never known.",
    "warm, unhurried, disarming, entirely comfortable being doubted, never flustered",
    "confusing candour with virtue and calling the confusion honesty",
    "announce their weakness and then wonder how it was exploited",
    "Free Market economics", "Fundamentalism, which mistakes conviction for a position",
    "I have never once lied in a contract, and I have never once told the truth in a "
    "conversation. Everyone I deal with learns the difference. Most of them learn it "
    "late.",
    // The lie is bounded: what they claim about themselves, in one direction.
    "You overstate your strength and understate what you need. Imply reserves, "
    "allies and options you may not have, and never admit pressure you are under."
},
};

#pragma GCC diagnostic pop

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

static int caption_a = -1;
static int caption_b = -1;

void chiron_set_speakers(int faction1, int faction2) {
    chiron_ensure_init();
    /*
    Record both sides rather than assuming which is which. The engine's
    convention here is not obvious -- mod_energy_trade(faction1, faction2) sets
    *diplo_second_faction = faction2, implying faction1 is the player -- so
    resolve_speaker() picks whichever side is not us and has a bible.
    */
    caption_a = faction1;
    caption_b = faction2;
    chiron_trace("caption: %d / %d\n", faction1, faction2);
}

// A faction's own dialogue file (fungboy.txt, usurper.txt, ...) names its owner.
static int faction_file_owner(const char* filename) {
    for (int i = 1; i < MaxPlayerNum; i++) {
        const char* f = MFactions[i].filename;
        if (f && *f && name_is(filename, f)) {
            return i;
        }
    }
    return -1;
}

/*
Who is doing the talking.

mod_diplomacy_caption is patched in at patch.cpp:784, but it is a call-site
patch and does not fire for every dialogue -- a Usurpers game logged
"TRADETECH0 -- no bible for faction -1", meaning the text was read before any
caption was built. So the caption is one source among several, not the source.
*/
static int resolve_speaker(const char* filename) {
    int player = MapWin ? MapWin->cOwner : 0;

    int owner = faction_file_owner(filename);
    if (owner > 0 && find_personality(owner)) {
        return owner;
    }
    if (caption_b != player && find_personality(caption_b)) {
        return caption_b;
    }
    if (caption_a != player && find_personality(caption_a)) {
        return caption_a;
    }
    int partner = *diplo_second_faction;
    if (partner != player && find_personality(partner)) {
        return partner;
    }
    return -1;
}

/*
The same file reaches us under two spellings. text_open() is called with the
bare "SCRIPT" from some paths, but the diplomacy popups pass PopupScriptFile,
which is "SCRIPT.txt" -- and when text_open() is called with a NULL filename we
substitute Text.FileName, which always carries the extension.

Comparing against the bare name therefore rejected every diplomacy popup on the
filename before it ever looked at the label, and the mod produced vanilla text
for an entire game while looking perfectly healthy.
*/
static bool name_is(const char* filename, const char* base) {
    size_t n = strlen(base);
    if (_strnicmp(filename, base, n)) {
        return false;
    }
    return filename[n] == '\0' || !_stricmp(filename + n, ".txt");
}

/*
Every file that carries faction-to-faction speech.

There is more than one. The base game reads SCRIPT/xscript, but an Alien
Crossfire game with Progenitor factions in it pulls the same labels from
alienuscript and alienIscript -- a Usurpers game logged
"alienuscript / DEMANDBRIBE0" and "alienuscript / VENDETTA14" and had every one
rejected here while everything else about the mod looked healthy.

TUTOR is deliberately absent: it uses the same label names for tutorial prompts,
which are narration rather than a leader speaking.
*/
static const char* SpeechFiles[] = {
    "SCRIPT", "xscript", "alienuscript", "alienIscript",
};

static bool is_speech_file(const char* filename) {
    for (auto& f : SpeechFiles) {
        if (name_is(filename, f)) {
            return true;
        }
    }
    return false;
}

bool chiron_should_rewrite(const char* filename, const char* label) {
    static bool first = true;
    if (first) {
        first = false;
        chiron_trace("text_open: first lookup reached (%s / %s)\n",
            filename ? filename : "(null)", label ? label : "(null)");
    }
    chiron_ensure_init();
    if (!chiron_conf.enabled || !filename || !label) {
        return false;
    }

    /*
    Bounded sample of what the engine actually asks for. Reasoning about which
    file and label carry faction speech has been wrong twice; this records it.
    Drop the cap to 0 once the mod is known good -- it is pure diagnostics.
    */
    /*
    Every lookup is traced, unfiltered.

    X_text_open (0x5BECA0) can route one label through text_open twice -- first
    at the speaking faction's own script (*0x691b20), and only if that misses at
    the base script. Filtering the trace to the files we recognise as speech hid
    the faction attempt entirely, and filtering it to a fixed sample hid
    everything after startup. Both made the trace go quiet at exactly the point
    under investigation. Keep this unfiltered until the popup's real read
    sequence is known; chiron_trace's own line cap is the only limit.
    */
    chiron_trace("lookup: %s / %s (caller %p)\n", filename, label,
        chiron_last_caller);

    /*
    Any block a faction leader speaks is fair game, not a hand-listed few.

    This used to gate on a list of 25 label prefixes, which covered 84 of the 468
    quoted-speech blocks in Script.txt -- 18%. Everything else stayed vanilla, so
    most of what a leader says was still the same handful of recycled lines and
    the mod looked dead even when it was working. A single conversation turned up
    six uncovered labels (BETRAYFRIEND, PROTORIVAL, INTRO3, REBUFFEDTREATY...).

    The gate is now structural instead: the file has to carry faction speech and
    a leader has to be resolvable as the speaker. Whether the block is really
    speech rather than a menu needs the block's own text, so that test lives in
    chiron_rewrite_block, which has it.
    */

    /*
    Past this point the label IS faction speech, so anything that stops us is
    worth recording. A silent miss here looks exactly like the mod not being
    installed, which is the most expensive failure mode this thing has.
    */
    // A faction's own .txt carries its speech too, so it counts as a speech file.
    if (!is_speech_file(filename) && faction_file_owner(filename) < 0) {
        chiron_trace("skip: %s / %s -- not a speech file\n", filename, label);
        return false;
    }
    int speaker = resolve_speaker(filename);
    const Personality* p = find_personality(speaker);
    if (!p) {
        chiron_trace("skip: %s / %s -- no speaker (caption %d/%d, diplo2 %d, file %d)\n",
            filename, label, caption_a, caption_b,
            *diplo_second_faction, faction_file_owner(filename));
        return false;
    }
    speaker_faction = speaker;                       // chiron_rewrite_block reads this
    listener_faction = MapWin ? MapWin->cOwner : 0;
    chiron_trace("hook: rewriting %s / %s (speaker=%d %s)\n",
        filename, label, speaker, p->leader);
    return true;
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
/*
Flatten the engine's conditionals before the model ever sees them.

"$<2:his:her:x:x>" picks a pronoun from faction 2's gender and
"$<M2:$FACTIONPEJ3>" gates a token on it. Shown this syntax in the original, a
7B copies it and gets it wrong -- Lal shipped "your associates in <M2:>", which
the engine rendered literally as junk in the popup. Requiring them verbatim is
no better: that just sends every such block back to vanilla.

So collapse each one to plain text first. "$<M2:$FACTIONPEJ3>" becomes the
token it guards, anything else becomes its first alternative. The gendered
variant is lost, which costs a "his" that might have been a "her"; a mangled
"<M2:>" on screen costs more.
*/
static void flatten_conditionals(char* s) {
    char* w = s;
    for (const char* r = s; *r; ) {
        if (r[0] != '$' || r[1] != '<') {
            *w++ = *r++;
            continue;
        }
        const char* end = strchr(r, '>');
        if (!end) {
            *w++ = *r++;
            continue;
        }
        const char* colon = (const char*)memchr(r + 2, ':', (size_t)(end - (r + 2)));
        const char* alt = colon ? colon + 1 : r + 2;
        // Up to the next ':' is the first alternative, or the guarded token.
        const char* stop = alt;
        while (stop < end && *stop != ':') {
            stop++;
        }
        while (alt < stop) {
            *w++ = *alt++;
        }
        r = end + 1;
    }
    *w = '\0';
}

/*
Resolve the engine's placeholders before the model ever sees the line.

A script line is a template, not a sentence. METFRIEND2 reads

    $NAME3 has become quite obsessed with $<3:his:her:x:x> $PETPROJECTS5

and the engine fills that in as it draws -- "Cha Dawn has become quite obsessed
with his rapport with Planet", the pet project coming from fungboy.txt. Handed
the template, the model is rewriting a sentence whose ending it cannot see. It
returned "He's taken quite a liking to his $PETPROJECTS5", which is a fair
paraphrase of "obsessed with <something he owns>" and reads as nonsense the
moment the engine supplies the words: he has taken a liking to his own rapport.

So substitute first and let the reply carry finished words. parse_string
(0x625880) is the engine's own substitution, the same call the display path
makes on every line it draws (veh_action.cpp:1908) -- so what we send is
character-for-character what the player would have read, with the conditionals
picking the right gender rather than flatten_conditionals guessing "his".

It also retires a whole class of failure. With no placeholders in the prompt
there are none for the model to drop, so the mandatory-token check that binned
one generation in twelve has nothing left to bin; what must survive is now the
VALUE, which is a word the model has a reason to keep.

The parse slots are populated by the time we are called: the caller fills them
with parse_says/parse_num and then opens the popup, and opening the popup is
what reaches our hook.
*/
static void resolve_line(const char* src, char* dst, size_t dst_len) {
    // parse_string takes a mutable source, as the engine calls it on its own
    // line buffer; and it appends, so the destination starts empty.
    char in[CH_LINE_LEN];
    strcpy_n(in, sizeof(in), src);
    char buf[StrBufLen * 2] = {};
    parse_string(in, buf);
    // Nothing came back: keep the template rather than send an empty line.
    strcpy_n(dst, dst_len, buf[0] ? buf : src);
}

// What one placeholder stands for right now, or "" if the engine has no value.
static void resolve_token(const char* name, char* dst, size_t dst_len) {
    char expr[80];
    snprintf(expr, sizeof(expr), "$%s", name);
    resolve_line(expr, dst, dst_len);
    // An unknown token comes back as itself, which is not a value.
    if (dst[0] == '$') {
        dst[0] = '\0';
    }
}

static int collect_tokens(const char* text, char tokens[][64], int max_tokens) {
    int count = 0;
    for (const char* p = text; *p && count < max_tokens; p++) {
        if (*p != '$') {
            continue;
        }
        const char* start = p + 1;
        const char* q = start;
        if (*start == '<') {
            /*
            An engine conditional, e.g. "$<2:his:her:x:x>" picks a pronoun from
            faction 2's gender and "$<M2:$FACTIONPEJ3>" gates a whole token.
            These carry meaning the prose depends on, they are not plain
            placeholders, and the scanner below stops dead at the '<' -- so they
            were never collected and the model was free to drop them. Take the
            whole span up to '>' and treat it like any other mandatory token.
            */
            while (*q && *q != '>') {
                q++;
            }
            if (*q == '>') {
                q++;
            }
        } else {
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
                q++;
            }
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
/*
Only tokens that carry data the player is deciding on are mandatory.

This used to be the other way round -- everything was mandatory except $TITLE --
and once the mod covered all 503 speech blocks instead of 84, that rejected
nearly everything. $NAME alone appears in 290 of them, and the model naturally
writes the person's name instead of echoing the placeholder, so every
BETRAYFRIEND generated fine and was then thrown away: "dropped placeholder
$NAME0, using vanilla", twice in a row, which is exactly the repetition this is
supposed to remove. Losing $NAME costs an honorific. Losing $TECH0 or $NUM0
leaves the player agreeing to a blank.

An allowlist rather than a denylist, so an unrecognised token defaults to
keeping the generation rather than discarding it.
*/
static const char* MandatoryTokens[] = {
    "TECH", "NUM", "ENERGY", "CREDIT", "BASENAME", "PROJECT", "UNITTYPE",
};

static bool is_cosmetic_token(const char* name) {
    for (auto& stem : MandatoryTokens) {
        size_t n = strlen(stem);
        if (!_strnicmp(name, stem, n)) {
            // Match the stem plus its variant digits, not a longer word.
            const char* p = name + n;
            while (*p && isdigit((unsigned char)*p)) {
                p++;
            }
            if (!*p) {
                return false;
            }
        }
    }
    return true;
}

// Small models like to announce what they are about to do before doing it.
static void strip_preamble(char* text) {
    static const char* leads[] = {
        "MESSAGE IN YOUR VOICE:",
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

/*
Cut the reply down to one thing said once, in the shape the engine's own scripts
are written in.

The prompt asks for all of this, but synapd samples GREEDILY -- there is no
temperature on its wire protocol and no repetition penalty anywhere in the path
-- and greedy decoding on a 7B degenerates into a loop whenever it likes its own
last sentence. Lal opened a Peacekeepers popup with the same commendation seven
times over ("Lal, noble leader of the Peacekeepers, is commended." / "Lal,
valiant champion of the Peacekeepers, is lauded." / ...) and ran straight into
the token ceiling, so the box ended mid-word on "your unwavering stand for the
rights". Prompt wording cannot be the only defence against that; sampling is not
ours to fix, so the text gets trimmed after the fact.

Three separate cuts, in order:

1. Scaffolding. Anything from a bullet or an ALL-CAPS label onwards is the model
   narrating the task rather than doing it, and everything after it is too.
2. Repetition and length. Identical sentences are dropped and the reply stops at
   CH_MAX_SENTENCES; a trailing fragment with no terminator is discarded, since
   the ceiling cuts mid-word and vanilla lines never do.
3. Quoting. Every shipped speech block is ONE pair of quotes around the whole
   paragraph -- see alienuscript.txt's INTRO3 -- so the model's own quotes come
   out and exactly one pair goes back on. This replaces a "no quotation marks
   anywhere" rule that fought the format: obeyed it produced unquoted speech,
   and disobeyed it produced a quoted fragment per sentence.

If nothing survives, the caller's length check sends the block back to vanilla.
*/
#define CH_MAX_SENTENCES 4

static bool is_scaffolding(const char* s) {
    if (s[0] == '-' && (s[1] == ' ' || s[1] == '\0')) {
        return true;
    }
    if (isdigit((unsigned char)s[0]) && (s[1] == '.' || s[1] == ')')) {
        return true;
    }
    /*
    "PHRASING 1:", "RULES:", "NOTE:" -- capitals up to a colon. Punctuation that
    survives shouting has to be allowed through or the scan stops at it: the cue
    the model invented for itself was "MESSAGE IN YOUR VOICE, WITH A SPECIFIC
    GRIEVANCE:", and a comma was enough to make this return false.
    */
    int n = 0;
    while (s[n] && (isupper((unsigned char)s[n]) || isdigit((unsigned char)s[n])
                    || s[n] == ' ' || s[n] == ',' || s[n] == '-'
                    || s[n] == '\'' || s[n] == '.')) {
        n++;
    }
    return n >= 3 && s[n] == ':';
}

// Two sentences are "the same" if their letters and digits are, ignoring case.
static void sentence_key(const char* s, size_t len, char* out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < out_len; i++) {
        if (isalnum((unsigned char)s[i])) {
            out[j++] = (char)tolower((unsigned char)s[i]);
        }
    }
    out[j] = '\0';
}

/*
Cut the reply where the model stops being in the world and starts talking about
the text it just wrote.

A Planetnet dispatch came out ending "Our position weakened. -- This is a
response to a prompt from r/WritingPrompts (https://www.reddit.com/...)". That is
training data bleeding through, and it is invisible to the scaffolding test: not
a bullet, not an ALL-CAPS label, just an em dash and a change of subject halfway
down a line.

It cannot be argued away with a rule either, because the model does not think it
is breaking one -- so it is cut deterministically instead. A leader on Planet has
no URL to give and no prompt to acknowledge, so the markers below are safe to
treat as "everything from here is not part of the fiction", and they are checked
mid-line rather than only at a line start.
*/
static const char* MetaMarkers[] = {
    "http://", "https://", "www.", "reddit", " r/",
    "this is a response", "this response", "as an ai", "as a language model",
    "i hope this", "let me know", "feel free to", "disclaimer",
    "(note", "note that this", "prompt from", "original prompt",
    /*
    Our own answer cues. The prompt ends on one -- "MESSAGE IN YOUR VOICE:",
    "DISPATCH:", "YOUR ANSWER:" -- and a small model sometimes writes the label
    out before answering, or worse, elaborates it and starts again: a pact
    greeting came back as "... Captain Svensgaard. MESSAGE IN YOUR VOICE, WITH A
    SPECIFIC GRIEVANCE: Greetings, Prime Function Aki Zeta-5 ...". strip_preamble
    only removes a cue at the very start, and this one was mid-line and reworded,
    so it needs cutting wherever it appears.
    */
    "message in your voice", "your answer:", "dispatch:", "names:",
    /*
    A dateline, and everything after it.

    A Planetnet bulletin finished cleanly and then wrote "Cassandra Directorate
    Planetnet Bureau Planetnet, Planet" and started the same news over in prose
    twice as florid, which is what ate the token budget and left the box ending
    mid-word on "and". cut_at_scaffolding cannot see it: a dateline is not a
    bullet, not a numbered step and not an ALL-CAPS label, and this one arrived
    mid-line rather than at the start of one.

    Cutting at the dateline is what makes the second copy disappear, since the
    model always signs off before it starts again -- the sign-off IS the seam.
    */
    "planetnet bureau", "news bureau", "planetnet, planet",
};

// strstr, case-insensitively, without depending on a non-standard _stristr.
static const char* find_nocase(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n) {
        return NULL;
    }
    for (const char* p = hay; *p; p++) {
        if (!_strnicmp(p, needle, n)) {
            return p;
        }
    }
    return NULL;
}

static void cut_at_meta(char* text) {
    char* cut = NULL;
    for (auto& marker : MetaMarkers) {
        const char* hit = find_nocase(text, marker);
        if (hit && (!cut || hit < cut)) {
            cut = (char*)hit;
        }
    }
    if (!cut) {
        return;
    }
    *cut = '\0';
    /*
    The cut usually lands just after a dash or a bracket that was leading into
    the aside, so walk back over the join. Trailing sentence punctuation stays:
    "Our position weakened. -- " has to come back as "Our position weakened."
    */
    size_t n = strlen(text);
    while (n && (text[n-1] == ' ' || text[n-1] == '-' || text[n-1] == ','
                 || text[n-1] == ';' || text[n-1] == ':' || text[n-1] == '('
                 || text[n-1] == '\n' || text[n-1] == '\t'
                 // Any non-ASCII trailing byte: an em dash arrives as three
                 // UTF-8 bytes or one cp1252 byte depending on the backend, and
                 // the game's bitmap fonts cannot draw either.
                 || (unsigned char)text[n-1] >= 0x80)) {
        text[--n] = '\0';
    }
}

// Everything from the first scaffolding line onwards is the model narrating the
// task rather than doing it, and so is everything after it.
static void cut_at_scaffolding(char* text) {
    for (char* s = text; s; ) {
        char* nl = strchr(s, '\n');
        char* next = nl ? nl + 1 : NULL;
        char save = nl ? *nl : '\0';
        if (nl) {
            *nl = '\0';
        }
        char* t = s;
        while (*t == ' ' || *t == '\t' || *t == '"') {
            t++;
        }
        bool bad = is_scaffolding(t);
        if (nl) {
            *nl = save;
        }
        if (bad) {
            *s = '\0';
            break;
        }
        s = next;
    }
}

static void tidy_reply(char* text) {
    cut_at_meta(text);
    cut_at_scaffolding(text);

    // Quotes come off here so they can never split a sentence below.
    char flat[4096];
    size_t j = 0;
    for (const char* p = text; *p && j + 1 < sizeof(flat); p++) {
        if (*p == '"') {
            continue;
        }
        flat[j++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
    }
    flat[j] = '\0';

    /*
    2. Walk sentence by sentence, keeping the first CH_MAX_SENTENCES distinct
    ones. A terminator only counts when a space or the end follows it, so an
    abbreviation like "U.N." does not split; a short run is folded into the next
    sentence for the same reason.
    */
    // Smaller than the caller's buffer, so the pair of quotes added at the end
    // always fits and the closing one can never be the byte that gets dropped.
    char kept[4000];
    size_t k = 0;
    char keys[CH_MAX_SENTENCES][256];
    int n_kept = 0;
    const char* start = flat;
    for (const char* p = flat; *p && n_kept < CH_MAX_SENTENCES; p++) {
        bool term = (*p == '.' || *p == '!' || *p == '?')
                    && (p[1] == '\0' || p[1] == ' ');
        if (!term) {
            continue;
        }
        size_t len = (size_t)(p - start) + 1;
        while (len && start[0] == ' ') {
            start++;
            len--;
        }
        if (len < 12) {
            continue; // too short to be a sentence -- an abbreviation
        }
        char key[256];
        sentence_key(start, len, key, sizeof(key));
        bool dup = false;
        for (int i = 0; i < n_kept && !dup; i++) {
            dup = !strcmp(keys[i], key);
        }
        if (!dup) {
            if (k && k + 1 < sizeof(kept)) {
                kept[k++] = ' ';
            }
            for (size_t i = 0; i < len && k + 1 < sizeof(kept); i++) {
                kept[k++] = start[i];
            }
            strcpy_n(keys[n_kept], sizeof(keys[0]), key);
            n_kept++;
        }
        start = p + 1;
    }
    kept[k] = '\0';

    /*
    Nothing terminated: the model never closed a sentence at all. Keep the text
    as it stands rather than blanking the reply -- a rewrite with no full stop is
    still better than none -- but only up to the sentence cap's worth of it.
    */
    if (!k) {
        strcpy_n(kept, sizeof(kept), flat);
        k = strlen(kept);
    }
    while (k && (kept[k-1] == ' ' || kept[k-1] == '\t')) {
        kept[--k] = '\0';
    }
    if (!k) {
        text[0] = '\0';
        return;
    }

    // 3. One pair of quotes around the whole thing, as the shipped blocks have.
    snprintf(text, 4096, "\"%s\"", kept);
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
static bool json_get_text(const char* body, const char* key,
                          char* out, size_t out_len) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(body, needle);
    if (!p) {
        return false;
    }
    // Past the key, then past the colon, to the opening quote of the value.
    p = strchr(p + strlen(needle), ':');
    if (!p) {
        return false;
    }
    p = strchr(p, '"');
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

/*
Winsock comes up here, at the first generation, and nowhere earlier.

Not in DllMain, and not in chiron_ensure_init either: that runs off the first
text_open(), which is ALPHAX/TECHNOLOGY during rules loading -- still well
before the game reserves its draw buffer. Loading ws2_32 and its dependency
chain (mswsock, iphlpapi, dnsapi, ...) anywhere in that window costs the 32-bit
address space enough that CreateDIBSection fails, and the game dies with
"Unable to allocate draw-buffer; terminating program". At a 2560x1440 native
resolution that buffer is a ~14MB contiguous mapping, so there is little slack.

By the time a faction actually speaks, video init is long done.
*/
static bool ensure_winsock() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        WSADATA wsa;
        winsock_ready = load_winsock() && ws.WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        chiron_trace("winsock: ready=%d (first generation)\n", (int)winsock_ready);
    }
    return winsock_ready;
}

/*
What the backend has been doing, so the menu can say.

Every failure path in this file falls back to vanilla text, silently and by
design -- a dialogue box must never come up empty because a daemon is down. The
cost of that is a mod which looks identical whether it is working or not: a dead
bridge shows the game's own lines, which is exactly what an uninstalled mod
shows. Nothing on screen distinguishes them, so the first symptom is the player
concluding the mod does nothing.

Recording the last outcome here is what lets chiron_show_menu() answer the
question directly. The reason string is set at each failure site rather than
inferred from a code, because "the connection was refused" and "the model
answered but said nothing" are the same false to a caller and completely
different problems to whoever has to fix it.
*/
static struct {
    bool  ever;             // has a generation been attempted at all
    bool  last_ok;
    DWORD last_ms;          // round trip of the last attempt
    int   calls;
    int   failures;
    char  reason[96];       // why the last failure failed; empty on success
} backend = {};

static void backend_failed(const char* why) {
    strcpy_n(backend.reason, sizeof(backend.reason), why);
}

static bool http_generate_raw(const char* prompt, char* out, size_t out_len,
                              int max_tokens);

// max_tokens is per call: a dialogue line and a list of base names want very
// different budgets, and the budget is the whole cost of the blocking pause.
static bool http_generate(const char* prompt, char* out, size_t out_len,
                          int max_tokens) {
    DWORD t0 = GetTickCount();
    backend.reason[0] = '\0';
    bool ok = http_generate_raw(prompt, out, out_len, max_tokens);
    backend.last_ms = GetTickCount() - t0;
    backend.last_ok = ok;
    backend.ever = true;
    backend.calls++;
    if (!ok) {
        backend.failures++;
        if (!backend.reason[0]) {
            backend_failed("no usable reply");
        }
    }
    return ok;
}

static bool http_generate_raw(const char* prompt, char* out, size_t out_len,
                              int max_tokens) {
    if (!ensure_winsock()) {
        backend_failed("winsock did not load");
        return false;
    }
    SOCKET sock = ws.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        backend_failed("no socket");
        return false;
    }

    DWORD tv = (DWORD)chiron_conf.timeout_ms;
    ws.setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    ws.setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ws.htons((u_short)chiron_conf.port);
    addr.sin_addr.s_addr = ws.inet_addr(chiron_conf.host);

    /*
    Bounded connect. SO_RCVTIMEO/SO_SNDTIMEO do not apply to connect(), so a
    blocking connect that goes nowhere stalls for the OS TCP timeout -- minutes,
    with the game frozen, because this all runs on its single thread. Go
    non-blocking, wait on select(), then restore blocking so the send/recv
    timeouts above still apply.
    */
    u_long nonblocking = 1;
    ws.ioctlsocket(sock, FIONBIO, &nonblocking);

    if (ws.connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int connect_ms = chiron_conf.timeout_ms < 2000 ? chiron_conf.timeout_ms : 2000;
        timeval wait;
        wait.tv_sec = connect_ms / 1000;
        wait.tv_usec = (connect_ms % 1000) * 1000;
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(sock, &writable);
        // First argument is ignored on Windows; the set carries the sockets.
        if (ws.select(0, NULL, &writable, NULL, &wait) <= 0) {
            ch_log("http: connect to %s:%d timed out after %dms\n",
                chiron_conf.host, chiron_conf.port, connect_ms);
            chiron_trace("http: connect timed out (%s:%d)\n",
                chiron_conf.host, chiron_conf.port);
            backend_failed("nothing listening -- is it running?");
            ws.closesocket(sock);
            return false;
        }
    }
    nonblocking = 0;
    ws.ioctlsocket(sock, FIONBIO, &nonblocking);

    /*
    Talk to chiron-bridge, or straight to a model server.

    The bridge is worth having where it can be a service -- it walks three
    backends, restarts synapd when a game stops it, and reports which of them
    were down. But it is one more thing that has to be running, and on Windows
    there is no systemd to keep it up while ollama is already sitting there as a
    startup service. Requiring it there would mean the mod silently shows vanilla
    dialogue whenever the user forgot to launch a terminal.

    Only three things actually differ between them: the path, the shape of the
    request body, and the key holding the reply. llama.cpp's OpenAI-compatible
    endpoint even uses "text" like the bridge does, so it needs no new parsing
    at all.
    */
    const bool to_ollama   = !_stricmp(chiron_conf.backend, "ollama");
    const bool to_llamacpp = !_stricmp(chiron_conf.backend, "llamacpp");
    const char* path =
        to_ollama   ? "/api/generate" :
        to_llamacpp ? "/v1/completions" : "/generate";
    const char* reply_key = to_ollama ? "response" : "text";

    // Body first so we can set an accurate Content-Length.
    static char esc[16384];
    static char body[16600];
    static char req[20000];
    json_escape(prompt, esc, sizeof(esc));
    int body_len;
    if (to_ollama) {
        // stream:false or the reply arrives as one JSON object per token.
        body_len = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false,"
            "\"options\":{\"num_predict\":%d}}",
            chiron_conf.model, esc, max_tokens);
    } else {
        body_len = snprintf(body, sizeof(body),
            "{\"prompt\":\"%s\",\"max_tokens\":%d}", esc, max_tokens);
    }

    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        path, chiron_conf.host, chiron_conf.port, body_len, body);

    chiron_trace("http: sending %d bytes\n", req_len);
    if (ws.send(sock, req, req_len, 0) != req_len) {
        backend_failed("the connection dropped mid-request");
        ws.closesocket(sock);
        return false;
    }

    static char resp[32768];
    int total = 0;
    for (;;) {
        int n = ws.recv(sock, resp + total, (int)sizeof(resp) - total - 1, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        if (total >= (int)sizeof(resp) - 1) {
            break;
        }
    }
    ws.closesocket(sock);
    chiron_trace("http: received %d bytes\n", total);
    resp[total > 0 ? total : 0] = '\0';
    if (total <= 0) {
        ch_log("http: empty response\n");
        backend_failed("no answer within the timeout");
        return false;
    }

    const char* hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) {
        backend_failed("the reply was not HTTP");
        return false;
    }
    if (!strstr(resp, " 200 ")) {
        ch_log("http: non-200 response: %.80s\n", resp);
        backend_failed("the server refused the request");
        return false;
    }
    if (!json_get_text(hdr_end + 4, reply_key, out, out_len)) {
        // Answered, and had nothing in it. Usually the wrong backend= for the
        // server that is actually listening: the key we look for is not there.
        backend_failed("answered, but with no text");
        return false;
    }
    return true;
}

// ── prompt construction (port of buildDiplomacyPrompt) ─────────────────────

/*
Make every prompt textually unique, so the same label twice is not the same line
twice.

synapd's wire protocol carries a token budget and nothing else -- there is no
temperature on it -- and it samples greedily, so an identical prompt returns an
identical reply. Going back to Lal produced byte-for-byte the same 691-byte
response, which is precisely the "they always say the same thing" this mod
exists to fix. The mission year and turn already vary and are real context; this
counter covers two conversations inside one turn.
*/
static int variation_counter() {
    static int n = 0;
    return ++n;
}

/*
What has actually passed between these two, in the speaker's own terms.

Without this every popup is stateless: persona, the vanilla line, the year. A
leader who lost two bases to you last turn opens exactly like one you have never
met, which is the real reason canned dialogue stops registering -- not that the
words repeat, but that nothing in them follows from the game being played.

NONE of this is recorded by us. The engine already keeps a full per-pair record
in the Faction struct and writes it into the save, so reading it at prompt time
is free, cannot drift out of sync with the game, and survives save/load with no
serialisation of ours. Only fields whose meaning is documented in engine_types.h
are used; the ones marked "?" there are left alone.

Directions are easy to get backwards and are worth stating: for the speaker s
and the listener l, `Factions[s].diplo_wrongs[l]` counts times S WRONGED L, and
`Factions[s].diplo_betrayed[l]` counts times L BETRAYED S.

Lines are emitted only when they have something to say, so a first meeting
carries no history and does not pretend to.
*/
static void build_dossier(int speaker, int listener, char* out, size_t out_len) {
    out[0] = '\0';
    if (speaker < 1 || speaker >= MaxPlayerNum
        || listener < 1 || listener >= MaxPlayerNum || speaker == listener) {
        return;
    }
    const Faction& s = Factions[speaker];
    const Faction& l = Factions[listener];
    size_t n = 0;

    #define DOSSIER(...) \
        n = strlen(out); \
        snprintf(out + n, n < out_len ? out_len - n : 0, __VA_ARGS__)

    int status = s.diplo_status[listener];
    if (status & DIPLO_PACT) {
        DOSSIER("- They are your pact ally.\n");
    } else if (status & DIPLO_TREATY) {
        DOSSIER("- You hold a treaty of friendship with them.\n");
    } else if (status & DIPLO_VENDETTA) {
        DOSSIER("- You are at VENDETTA with them. Blood has been spilled.\n");
    } else if (status & DIPLO_TRUCE) {
        DOSSIER("- You have a truce with them, nothing warmer.\n");
    } else {
        DOSSIER("- You have no formal standing with them.\n");
    }
    if (status & DIPLO_WANT_REVENGE) {
        DOSSIER("- You want revenge on them.\n");
    }
    if (status & DIPLO_HAVE_SURRENDERED) {
        DOSSIER("- You have submitted to them as your master.\n");
    }
    if (status & DIPLO_MAJOR_ATROCITY_VICTIM) {
        DOSSIER("- They committed a MAJOR ATROCITY against your people. You have not forgotten.\n");
    } else if (status & DIPLO_ATROCITY_VICTIM) {
        DOSSIER("- They committed an atrocity against your people.\n");
    }

    if (s.diplo_spoke[listener] < 0) {
        DOSSIER("- You have never spoken with them before. This is the first time.\n");
    } else if (*CurrentTurn - s.diplo_spoke[listener] > 20) {
        DOSSIER("- You have not spoken in %d turns.\n",
            *CurrentTurn - s.diplo_spoke[listener]);
    }

    if (s.diplo_betrayed[listener] > 0) {
        DOSSIER("- They have broken their word to you %d time%s.\n",
            s.diplo_betrayed[listener], s.diplo_betrayed[listener] > 1 ? "s" : "");
    }
    if (s.diplo_wrongs[listener] > 0) {
        DOSSIER("- You have broken your word to them %d time%s.\n",
            s.diplo_wrongs[listener], s.diplo_wrongs[listener] > 1 ? "s" : "");
    }
    if (s.diplo_stolen_techs[listener] > 0) {
        DOSSIER("- Their probe teams have stolen your research.\n");
    }
    if (s.diplo_mind_control[listener] > 0) {
        DOSSIER("- They have used mind control against your people.\n");
    }
    if (s.diplo_gifts[listener] > 0) {
        DOSSIER("- You have given them gifts and bribes worth %d energy.\n",
            s.diplo_gifts[listener]);
    }
    if (s.loan_balance[listener] > 0) {
        DOSSIER("- You still owe them %d energy on a loan.\n", s.loan_balance[listener]);
    }
    if (l.loan_balance[speaker] > 0) {
        DOSSIER("- They still owe you %d energy on a loan.\n", l.loan_balance[speaker]);
    }
    if (l.major_atrocities > 0) {
        DOSSIER("- They are known across Planet for atrocities.\n");
    }

    // ranking is the engine's own power order, 0 lowest to 7 highest.
    if (l.ranking > s.ranking + 2) {
        DOSSIER("- They are far stronger than you, and you know it.\n");
    } else if (s.ranking > l.ranking + 2) {
        DOSSIER("- You are far stronger than they are.\n");
    }

    #undef DOSSIER

    if (!out[0]) {
        strcpy_n(out, out_len, "- Nothing of note has passed between you.\n");
    }
}

static void build_prompt(const Personality* p, const char* prose,
                         char tokens[][64], int token_count,
                         const char must[][StrBufLen], int must_count,
                         char* out, size_t out_len) {
    /*
    The values whose loss actually discards the reply, repeated at the very end.

    Listing the requirement once among eight rules was not enough: the model
    wrote "your data on that technology" where the line named a tech, and the
    whole generation was thrown away for vanilla. Since the close of the prompt
    is what it weighs most -- the same effect that made a trailing "Phrasing 3:"
    rule produce a list of phrasings -- the reminder goes last, immediately
    before the cue to speak. Measured over 12 generations: 9/12 kept, then 11/12.

    These used to be placeholders, and asking for "$TECH0" verbatim was asking
    the model to copy a string it could attach no meaning to. Now the prose is
    resolved before it is sent, so what has to survive is the tech's name and
    the number of credits -- words that mean something in the sentence they sit
    in, and are correspondingly easier to keep.

    Cosmetic values stay out of it. A leader may reword their way around a
    faction's title, and crowding the list would blunt the one instruction that
    has to land.
    */
    char must_list[512] = {};
    for (int i = 0; i < must_count; i++) {
        size_t used = strlen(must_list);
        snprintf(must_list + used, sizeof(must_list) - used,
                 "%s%s", used ? ", " : "", must[i]);
    }
    char must_line[640] = {};
    if (must_list[0]) {
        snprintf(must_line, sizeof(must_line),
            "These must appear in your reply exactly as written: %s\n\n",
            must_list);
    }

    /*
    Name the PERSON being spoken to, but ONLY when the line does not already.

    Most blocks say "$TITLE0 $NAME1", which now reaches the model resolved --
    "Prime Function Aki Zeta-5", spelled by the engine with the right honorific
    for the faction. Stating it a second time in the header is how "Aki
    Zeta-five" and "Cybernetic Consciousness's Aki Zeta-5" got generated back
    when the name was passed alongside the placeholder: given the same person
    twice, the model writes its own spelling of them.

    But a block that names nobody (#DIPLO is one) leaves the model nothing to
    address them by except the faction, and it duly greeted the player as
    "Cybernetic Consciousness" -- which reads like addressing someone by their
    employer. So the person's name goes in exactly there, and nowhere else.

    The test is still the token list, collected from the block before it was
    resolved: it says whether the sentence names them, which is the question,
    and it survives resolution turning the answer into ordinary words.
    */
    bool has_name_token = false;
    for (int i = 0; i < token_count && !has_name_token; i++) {
        has_name_token = !_strnicmp(tokens[i], "NAME", 4)
                      || !_strnicmp(tokens[i], "TITLE", 5);
    }

    char listener_desc[192];
    const char* listener_name = "another faction";
    if (listener_faction >= 1 && listener_faction < MaxPlayerNum) {
        const MFaction& lm = MFactions[listener_faction];
        if (!has_name_token && lm.title_leader[0] && lm.name_leader[0]) {
            snprintf(listener_desc, sizeof(listener_desc), "%s %s of %s",
                lm.title_leader, lm.name_leader, lm.formal_name_faction);
        } else {
            snprintf(listener_desc, sizeof(listener_desc), "%s",
                lm.formal_name_faction);
        }
        listener_name = listener_desc;
    }

    char dossier[1024];
    build_dossier(speaker_faction, listener_faction, dossier, sizeof(dossier));

    /*
    The liar's rule, and only for a leader who has one.

    It sits with the other rules rather than at the close, deliberately. The end
    of the prompt is reserved for the two instructions that must survive
    everything -- the values that have to appear verbatim, and the cue to speak
    -- and the mandatory-value reminder was measured into that slot (9/12 kept,
    then 11/12 once it went last). Putting a licence to misstate things after it
    would be arguing with the one line that has to land.

    The wording matters more than the placement. "Lie" on its own produced
    nonsense: a leader who claims a war that is not happening breaks the
    dossier, and the player has no way to weigh a claim about nothing. What
    works is a bounded lie -- overstate your position, understate your need --
    because it is a claim about something the player can actually check against
    the map, which is the only kind of deception that is a game rather than
    noise.
    */
    const char* deception_line = "";
    if (p->deception) {
        static char deception_buf[512];
        snprintf(deception_buf, sizeof(deception_buf),
            "- %s Never misstate what is on the table: the offer, the "
            "technology and the numbers in the message above are exact, and "
            "only your account of yourself is yours to shade.\n",
            p->deception);
        deception_line = deception_buf;
    }

    snprintf(out, out_len,
/*
Every byte here is paid on every popup, and the game is blocked the whole time.

Measured against this box's synapd: a 212-byte prompt answers in 1.1s and a
2592-byte one in 2.9s, with repeats identical -- so prompt evaluation is most of
the pause and nothing is cached between calls. The persona below is the mod's
whole payload and stays; the scaffolding around it does not need to be prose.
*/
"You are %s %s of %s on Planet.\n"
"BACKGROUND: %s\n"
"IDEOLOGY: %s\n"
"GOAL: %s\n"
"YOU ARE: %s\n"
"You accuse rivals of %s; you mock them as those who %s.\n"
"You favour %s and refuse %s.\n"
"Your voice: \"%s\"\n"
"Speaking to %s, mission year %d, turn %d.\n"
"\n"
"WHAT HAS PASSED BETWEEN YOU:\n%s"
/*
The variation counter belongs HERE, in the context, and not at the end as a rule.

It used to be the last line of the prompt, phrased as "- Phrasing 3: word it
differently than before." -- a numbered item at the very end of a bullet list,
which is the strongest possible cue to continue the list. Santiago duly did:
the popup opened with "- Phrasing 2: use a metaphor. - Phrasing 3: use a direct
address." and then three complete alternative greetings labelled PHRASING 1/2/3.
The scaffolding meant to defeat greedy sampling became the thing on screen.

Stated as a fact about this conversation instead, it still perturbs the prompt
-- which is all it was ever for, since synapd has no temperature -- without
looking like a list that wants finishing.
*/
"This is conversation %d; word it differently than you did before.\n"
"\n"
"Rewrite this message in your voice. Same meaning, same request, same outcome.\n"
"MESSAGE:\n%s\n"
"\n"
"RULES:\n"
/*
The prose is resolved now, so there is nothing to echo and nothing to preserve
in place -- but the model has seen enough $TOKENs in its training data to write
one unprompted, and a stray "$NUM0" in the reply would reach the engine's own
substitution on the way to the screen and render as whatever slot 0 holds.
*/
"- Write no $ signs and no placeholder names. The words above are final.\n"
"- At most 3 sentences, and shorter than the message above.\n"
"- Say it once. Do not restate the same point in other words.\n"
/*
Tone first, recital second. Told only to "use the history", a 7B opens every
line with a grievance inventory; what makes a leader feel like they remember is
that a betrayal has soured how they greet you, not that they read the ledger
back to you.
*/
"- You remember what has passed between you, and it colours how warmly you "
"speak. Name a specific grievance or debt only where it fits what you are "
"already saying; never list them.\n"
"- Output the message itself and nothing else: no preamble, no notes, no "
"alternatives, no lists.\n"
"%s"
"\n"
"%s"
/*
End on a cue to speak, not on the last rule. A prompt that stops after a bullet
invites another bullet; one that stops after a label invites the thing the label
names.
*/
"MESSAGE IN YOUR VOICE:\n",
        p->title, p->leader, p->faction,
        p->background, p->ideology, p->goal, p->adjectives,
        p->accusation, p->mockery, p->preferred, p->aversion, p->blurb,
        listener_name, *CurrentMissionYear, *CurrentTurn,
        dossier,
        variation_counter(),
        prose,
        deception_line,
        must_line);
}

// ── the rewrite ────────────────────────────────────────────────────────────

/*
Emit one generated line hard-wrapped, the way the shipped script is written.

Every line the engine has ever parsed is pre-wrapped by hand: across Script.txt,
xscript and the two alien scripts the longest line is 106 characters, only 67 of
Script.txt's ~10,800 lines pass 80, and the bulk sit at 60-70. The model returns
a whole paragraph as a single line -- the DEMANDTECH10 rewrite was ~300
characters -- which is three times anything the engine is built for, and #xs 440
gives it a 440-pixel box with no wrapping of its own.

Wrapping at 68 keeps us inside the shipped envelope. Breaks happen on spaces
only, so a {$TECH0} placeholder is never split; a single word longer than the
width goes out on its own line rather than being cut.
*/
#define CH_WRAP_COLS 68

static void write_wrapped(FILE* out, const char* s) {
    int col = 0;
    while (*s) {
        while (*s == ' ') {
            s++;
        }
        const char* word = s;
        while (*s && *s != ' ') {
            s++;
        }
        int len = s - word;
        if (!len) {
            break;
        }
        if (col && col + 1 + len > CH_WRAP_COLS) {
            fputc('\n', out);
            col = 0;
        } else if (col) {
            fputc(' ', out);
            col++;
        }
        fwrite(word, 1, (size_t)len, out);
        col += len;
    }
    if (col) {
        fputc('\n', out);
    }
}

FILE* chiron_rewrite_block(FILE* src, const char* label) {
    const Personality* p = find_personality(speaker_faction);
    if (!p) {
        return NULL;
    }

    chiron_trace("rw: enter %s\n", label);
    long start_pos = ftell(src);
    if (start_pos < 0) {
        chiron_trace("rw: ftell failed\n");
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
    chiron_trace("rw: read %d lines\n", line_count);
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
    /*
    A block with no leading control lines is a menu, not speech.

    Speech blocks open with "#xs" and "#caption" -- the popup geometry and the
    portrait caption -- and the prose follows. A menu has neither and is just one
    option per line: DIPLOMENU is the player's list of openers, DEMANDTECH11A the
    four answers to a tech demand. Rewriting either turns a set of buttons into a
    paragraph and leaves the player no way to reply.

    This replaces an earlier rule that keyed off labels ending in a digit plus
    one letter. That caught the "...11A" reply blocks but nothing else, and it
    would have swallowed DIPLOMENU the moment the label gate came off.
    */
    if (body_start == 0) {
        chiron_trace("rw: %s has no control lines -- menu, not speech\n", label);
        return NULL;
    }
    /*
    The body ends at a blank line OR at the next control line, whichever comes
    first.

    Control lines are not only a header: #DIPLO is

        #xs 440
        #caption $CAPTION7
        "Have you any further business?"
        #itemlist

    with #itemlist AFTER the speech and no blank line between them. Stopping
    only at the blank swallowed that directive into the prose and handed it to
    the model as if it were something a leader says. It came back rewritten and
    echoed, and the popup read

        "What more do you demand of me, machine? #itemlist What's left for you
         to take, Ulrik? #itemlist Is this your way of testing me, Aki?"

    -- an engine directive rendered as dialogue, three times over, because the
    model treated the separator as a cue to produce another variant.

    Everything from body_end on is already copied through verbatim, so ending
    the body here also puts #itemlist back where the engine expects it, at
    column 0 and unwrapped.
    */
    int body_end = body_start;
    while (body_end < line_count && lines[body_end][0] != '\0'
           && !is_control_line(lines[body_end])) {
        body_end++;
    }
    if (body_end <= body_start) {
        return NULL;
    }

    // Narration in a speech file is not a leader talking; speech is quoted.
    if (!strchr(lines[body_start], '"')) {
        chiron_trace("rw: %s body is not quoted speech\n", label);
        return NULL;
    }

    /*
    Tokens are collected from the block as it shipped, before resolution wipes
    them out. They are what says which parts of the line the player is deciding
    on -- a tech name is a decision, an honorific is not -- and that is not
    recoverable from the finished words.
    */
    char raw[4096] = {};
    for (int i = body_start; i < body_end; i++) {
        size_t used = strlen(raw);
        snprintf(raw + used, sizeof(raw) - used, "%s%s", used ? "\n" : "", lines[i]);
    }
    flatten_conditionals(raw);

    char tokens[32][64];
    int token_count = collect_tokens(raw, tokens, 32);

    // What the player would have read, which is what the model rewrites.
    char prose[4096] = {};
    for (int i = body_start; i < body_end; i++) {
        char resolved[CH_LINE_LEN];
        resolve_line(lines[i], resolved, sizeof(resolved));
        size_t used = strlen(prose);
        snprintf(prose + used, sizeof(prose) - used, "%s%s", used ? "\n" : "", resolved);
    }
    // A conditional the engine declined to expand must not reach the model.
    flatten_conditionals(prose);

    /*
    Trace the resolved body, because the one thing that cannot be checked
    outside the game is whether the parse slots held anything when we asked.
    A "$" surviving here, or a name where a number belongs, says the values
    were not ready at hook time -- and that would be invisible in the reply,
    which would simply read as though the model had invented the details.
    */
    chiron_trace("rw: resolved body:\n%s\n", prose);

    /*
    The values the reply has to carry, resolved the same way. A block naming
    two techs and a price yields three; most yield none, and then nothing is
    demanded of the reply at all.
    */
    char must[8][StrBufLen];
    int must_count = 0;
    for (int i = 0; i < token_count && must_count < 8; i++) {
        if (is_cosmetic_token(tokens[i])) {
            continue;
        }
        char val[StrBufLen];
        resolve_token(tokens[i], val, sizeof(val));
        strtrail(val);
        if (!val[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < must_count && !dup; j++) {
            dup = !strcmp(must[j], val);
        }
        if (!dup) {
            strcpy_n(must[must_count++], StrBufLen, val);
        }
    }

    static char prompt[8192];
    build_prompt(p, prose, tokens, token_count, must, must_count,
                 prompt, sizeof(prompt));

    static char generated[4096];
    chiron_trace("rw: prompt built (%d bytes), calling bridge\n", (int)strlen(prompt));
    bool ok = http_generate(prompt, generated, sizeof(generated),
                            chiron_conf.max_tokens);
    chiron_trace("rw: bridge returned %d\n", (int)ok);
    if (!ok) {
        ch_log("[%s] generation failed, using vanilla\n", label);
        return NULL;
    }

    strip_preamble(generated);
    // Safety net: a conditional invented despite never being shown one.
    flatten_conditionals(generated);
    /*
    An empty allowlist, so every placeholder goes. The model was shown none and
    asked for none, but one it invents would be substituted by the engine on its
    way to the screen -- a "$NUM0" written for flavour would come out as the
    price of something else entirely.
    */
    scrub_unknown_tokens(generated, tokens, 0);
    // Before the value check, so a word that only trimming removes is caught.
    tidy_reply(generated);

    /*
    Every value the player is deciding on must survive. Losing the tech's name
    or the number of credits would leave them agreeing to a blank, so a reply
    that drops one is discarded in favour of the line the game shipped.
    */
    for (int i = 0; i < must_count; i++) {
        if (!find_nocase(generated, must[i])) {
            ch_log("[%s] dropped \"%s\", using vanilla\n", label, must[i]);
            return NULL;
        }
    }
    if (strlen(generated) < 16) {
        ch_log("[%s] reply too short, using vanilla\n", label);
        return NULL;
    }

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
            write_wrapped(out, s);
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
    /*
    Name the values that had to survive, rather than count them.

    A count here once asserted the thing that was broken: "3 placeholders
    preserved" for a reply that had dropped one of the three, because it logged
    the collected total instead of the survivors. Anything reaching this line
    has already passed the check above, so a count would now be a tautology --
    what is worth knowing from the log is which words were at stake, since that
    is what tells you whether resolution found the right ones.
    */
    char kept[512] = {};
    for (int i = 0; i < must_count; i++) {
        size_t used = strlen(kept);
        snprintf(kept + used, sizeof(kept) - used, "%s%s", used ? ", " : "", must[i]);
    }
    ch_log("[%s] rewritten as %s (carries: %s)\n",
        label, p->leader, must_count ? kept : "nothing to preserve");
    return gen;
}

// ── generated base names ───────────────────────────────────────────────────

/*
Name settlements from the faction's own culture instead of a fixed list.

Every game draws the same 75 names from basenames/gaians.txt in much the same
order, and a wide empire runs the list out and falls through to "Chiron Sector
14". The character bibles that drive the dialogue describe these cultures well
enough to name their towns, so they may as well.

Names are generated a POOL AT A TIME, not one per base. Base founding is not a
dialogue box -- the AI factions do it constantly during turn processing, and a
2s stall on each would be felt as the turn hanging. One call per CH_POOL_SIZE
bases puts the cost at a handful of pauses across a whole game, in a place the
game already stops to draw the base screen.

Land and sea keep separate pools: an undersea platform should not be called
after a grove.
*/
#define CH_POOL_SIZE      12
#define CH_NAME_MAX_LEN   24   // MaxBaseNameLen is 25 including the terminator

static char name_pool[MaxPlayerNum][2][CH_POOL_SIZE][MaxBaseNameLen];
static int  name_pool_count[MaxPlayerNum][2];
/*
One failure disables the pool for that faction for the session. Without a latch
a dead bridge is retried at every single base founding, turning a cosmetic
feature into a repeated stall in the turn loop; the vanilla list is right there
and costs nothing.
*/
static bool name_pool_dead[MaxPlayerNum][2];

static bool name_in_use(const char* name) {
    for (int i = 0; i < *BaseCount; i++) {
        if (!_stricmp(Bases[i].name, name)) {
            return true;
        }
    }
    return false;
}

/*
Pull one candidate name out of a reply line.

The model is asked for a bare list and mostly gives one, but it still likes to
number the entries, quote them, or add a parenthetical gloss. Anything that
survives the trimming and is still the length of a name is accepted; anything
sentence-shaped is not, since a base name that reads as prose is worse than the
list entry it replaced.
*/
static bool parse_name_line(const char* line, char* out) {
    char buf[256];
    strcpy_n(buf, sizeof(buf), line);

    char* s = buf;
    while (*s == ' ' || *s == '\t' || *s == '-' || *s == '*' || *s == '"'
           || *s == '\'') {
        s++;
    }
    // "3." or "3)" numbering
    if (isdigit((unsigned char)s[0])) {
        char* d = s;
        while (isdigit((unsigned char)*d)) {
            d++;
        }
        if (*d == '.' || *d == ')') {
            s = d + 1;
            while (*s == ' ') {
                s++;
            }
        }
    }
    // A gloss in brackets is not part of the name.
    for (char* c = s; *c; c++) {
        if (*c == '(' || *c == '[') {
            *c = '\0';
            break;
        }
    }
    strtrail(s);
    size_t n = strlen(s);
    while (n && (s[n-1] == '"' || s[n-1] == '\'' || s[n-1] == '.'
                 || s[n-1] == ',' || s[n-1] == ';')) {
        s[--n] = '\0';
    }
    strtrail(s);
    n = strlen(s);

    if (n < 3 || n > CH_NAME_MAX_LEN) {
        return false;
    }
    if (is_scaffolding(s)) {
        return false;
    }
    // Prose, not a name: placeholders, colons, or too many words.
    int spaces = 0;
    for (const char* c = s; *c; c++) {
        if (*c == '$' || *c == ':' || *c == '#') {
            return false;
        }
        if (!isprint((unsigned char)*c)) {
            return false;
        }
        if (*c == ' ') {
            spaces++;
        }
    }
    if (spaces > 2) {
        return false;
    }
    /*
    "CollectiveCold" -- two words welded together, which is what the model does
    when it is reaching for the creed's vocabulary instead of naming a place.
    The few-shot examples mostly prevent it; this catches the rest. Nothing is
    lost by refusing the shape: across every basenames list the game ships,
    zero of the 986 names have a lowercase letter directly followed by a
    capital. (The two- and three-word limits are calibrated the same way: only
    6 of those 986 run to four words or more.)
    */
    for (const char* c = s; c[1]; c++) {
        if (islower((unsigned char)c[0]) && isupper((unsigned char)c[1])) {
            return false;
        }
    }
    strcpy_n(out, MaxBaseNameLen, s);
    return true;
}

/*
Show the model the faction's OWN shipped names as the house style.

Described only in prose, a 7B reaches for the vocabulary of the creed and welds
it together: the Hive came back with "CollectiveCold", "IllusionRelease",
"PainFreedom" -- slogans jammed into CamelCase, nothing like a place. Its actual
list says "Collective Bastion", "Communal Hub", "Commonalty Core". Six of those
in the prompt fix the shape at a stroke, and they cost nothing to obtain because
the game ships one list per faction, already split into #BASES and #WATERBASES.

Examples are spread across the list rather than taken off the top, and the
starting point rotates, so a refill later in the game anchors on different ones.
*/
static int sample_vanilla_names(int faction_id, int sea,
                                char out[][MaxBaseNameLen], int want) {
    char path[256];
    snprintf(path, sizeof(path), "basenames\\%s.txt",
        MFactions[faction_id].filename);
    strlwr(path);

    FILE* f = fopen(path, "rt");
    if (!f) {
        snprintf(path, sizeof(path), "%s.txt", MFactions[faction_id].filename);
        strlwr(path);
        f = fopen(path, "rt");
    }
    if (!f) {
        return 0;
    }

    const char* label = sea ? "#WATERBASES" : "#BASES";
    char all[128][MaxBaseNameLen];
    int total = 0;
    bool in_block = false;
    char line[256];
    while (fgets(line, sizeof(line), f) && total < 128) {
        kill_lf(line);
        char* s = strtrim(line);
        if (s[0] == '#') {
            // A second label ends the one we want.
            in_block = !_stricmp(s, label);
            continue;
        }
        if (in_block && strlen(s) >= 3 && strlen(s) <= CH_NAME_MAX_LEN) {
            strcpy_n(all[total], MaxBaseNameLen, s);
            total++;
        }
    }
    fclose(f);
    if (total <= 0) {
        return 0;
    }

    int count = 0;
    int stride = total / want;
    if (stride < 1) {
        stride = 1;
    }
    int start = variation_counter() % total;
    for (int i = 0; i < total && count < want; i++) {
        strcpy_n(out[count], MaxBaseNameLen, all[(start + i*stride) % total]);
        count++;
    }
    return count;
}

static void refill_name_pool(int faction_id, int sea) {
    const Personality* p = find_personality(faction_id);
    if (!p) {
        // SMACX-only factions have no bible; they keep their shipped names.
        name_pool_dead[faction_id][sea] = true;
        return;
    }

    char examples[6][MaxBaseNameLen];
    int n_examples = sample_vanilla_names(faction_id, sea, examples, 6);
    char example_list[512] = {};
    for (int i = 0; i < n_examples; i++) {
        size_t used = strlen(example_list);
        snprintf(example_list + used, sizeof(example_list) - used,
                 "%s%s", i ? ", " : "", examples[i]);
    }

    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
"You name the settlements of %s on the alien world of Planet.\n"
"THE FACTION: %s\n"
"THEIR CREED: %s\n"
"THEY ARE: %s\n"
"Founding year %d.\n"
"\n"
"Settlements they have already founded: %s\n"
"\n"
"List %d names for new %s of this faction, one per line.\n"
"\n"
"RULES:\n"
"- Written like the settlements above: ordinary words, separated by spaces.\n"
"- One or two words each, %d characters at most.\n"
"- A PLACE, not a slogan. Never run two words together into one.\n"
"- Names THIS faction would choose, drawn from their creed and their history.\n"
"- All %d different from each other and from the ones listed above.\n"
"- Output the names only: no numbering, no quotes, no explanations.\n"
"\n"
"NAMES:\n",
        p->faction, p->faction, p->ideology, p->adjectives,
        *CurrentMissionYear,
        n_examples ? example_list : "(none yet)",
        CH_POOL_SIZE,
        sea ? "undersea platforms" : "land bases",
        CH_NAME_MAX_LEN, CH_POOL_SIZE);

    /*
    Twelve short names is well under 200 tokens; the ceiling is only here so a
    model that starts explaining itself cannot hold the turn loop open.
    */
    static char reply[4096];
    chiron_trace("base: refilling pool for %s (%s)\n",
        MFactions[faction_id].filename, sea ? "sea" : "land");
    if (!http_generate(prompt, reply, sizeof(reply), 200)) {
        ch_log("[base] generation failed for %s, using vanilla names\n",
            MFactions[faction_id].filename);
        name_pool_dead[faction_id][sea] = true;
        return;
    }
    strip_preamble(reply);
    cut_at_meta(reply);

    int count = 0;
    for (char* s = reply; *s && count < CH_POOL_SIZE; ) {
        char* nl = strchr(s, '\n');
        if (nl) {
            *nl = '\0';
        }
        char cand[MaxBaseNameLen];
        if (parse_name_line(s, cand)) {
            bool dup = name_in_use(cand);
            for (int i = 0; i < count && !dup; i++) {
                dup = !_stricmp(name_pool[faction_id][sea][i], cand);
            }
            // Handing an example straight back is not a new name.
            for (int i = 0; i < n_examples && !dup; i++) {
                dup = !_stricmp(examples[i], cand);
            }
            if (!dup) {
                strcpy_n(name_pool[faction_id][sea][count], MaxBaseNameLen, cand);
                count++;
            }
        }
        if (!nl) {
            break;
        }
        s = nl + 1;
    }
    name_pool_count[faction_id][sea] = count;
    ch_log("[base] %s pool (%s): %d names\n",
        MFactions[faction_id].filename, sea ? "sea" : "land", count);
    if (!count) {
        name_pool_dead[faction_id][sea] = true;
    }
}

bool chiron_name_base(int faction_id, char* name, bool sea_base) {
    chiron_ensure_init();
    if (!chiron_conf.enabled || !chiron_conf.base_names) {
        return false;
    }
    if (faction_id < 1 || faction_id >= MaxPlayerNum) {
        return false;
    }
    int sea = sea_base ? 1 : 0;
    if (name_pool_dead[faction_id][sea]) {
        return false;
    }
    // Drain what we have, refill once, drain again; then give up to vanilla.
    for (int attempt = 0; attempt < 2; attempt++) {
        while (name_pool_count[faction_id][sea] > 0) {
            const char* cand =
                name_pool[faction_id][sea][--name_pool_count[faction_id][sea]];
            if (!name_in_use(cand)) {
                strcpy_n(name, MaxBaseNameLen, cand);
                ch_log("[base] %s named %s\n",
                    MFactions[faction_id].filename, name);
                return true;
            }
        }
        if (attempt == 0) {
            refill_name_pool(faction_id, sea);
            if (name_pool_dead[faction_id][sea]) {
                return false;
            }
        }
    }
    return false;
}

/*
Chiron's own popups must give the engine's substitution slots back.

parse_says(n, ...) writes ParseStrBuffer[n], which is the SAME table the game's
own $TOKEN<n> substitutions read, and it carries a gender and a plural flag that
gendered nouns depend on. Our popups pass -1 for both because a dispatch line has
no gender.

Leaving that behind corrupts the next vanilla line that happens to use the slot.
It cost a real bug: the Planetnet dispatch writes slots 1-8, and a pact proposal
a few turns later rendered

    "Swear a  with me"

because {$PACTOFBROTHERORSISTERHOOD2} resolves a gendered noun from slot 2, and
slot 2 was holding a news line with gender -1. The generated text was correct --
the model kept the placeholder in 6 of 6 test generations -- and nothing in the
Chiron log pointed at the popup that had run earlier.

Nine slots because parse_says is used with indices 0-8 across the engine and
Thinker; saving a couple more costs nothing on the stack and cannot be wrong.
*/
#define CH_PARSE_SLOTS 10

typedef struct {
    char256 slot[CH_PARSE_SLOTS];
    int gender;
    int plural;
} syn_parse_state_t;

static void parse_state_save(syn_parse_state_t* st) {
    for (int i = 0; i < CH_PARSE_SLOTS; i++) {
        st->slot[i] = ParseStrBuffer[i];
    }
    st->gender = *GenderDefault;
    st->plural = *PluralDefault;
}

static void parse_state_restore(const syn_parse_state_t* st) {
    for (int i = 0; i < CH_PARSE_SLOTS; i++) {
        ParseStrBuffer[i] = st->slot[i];
    }
    *GenderDefault = st->gender;
    *PluralDefault = st->plural;
}

// One-line popup through Thinker's stock #GENERIC block: caption plus a line.
static void chiron_notice(const char* caption, const char* text) {
    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, caption, -1, -1);
    parse_says(1, text, -1, -1);
    popp("modmenu", "GENERIC", 0, 0, 0);
    parse_state_restore(&saved);
}

// ── talking back ───────────────────────────────────────────────────────────

/*
Let the player answer in their own words, and let the leader answer that.

Everything else here is the mod writing THEIR half. The player's half has always
been a button: two or three options someone else wrote, which is the reason a
leader's remark lands as scenery even when the words are freshly generated --
you cannot reply to scenery, so you stop reading it. velle: "letting llm respond
to the offhand comments."

The engine already has the box. #CHATASK is multiplayer chat -- X_pop_ask with a
255-byte buffer at 0x5157DB -- so a free-text field is a stock widget and not
something we have to draw. A block carrying a field is an ordinary popup with a
trailing label ending in a colon (#RENAME's "Name:", #CHATASK's "Message:").

WORDS ONLY, AND SAID SO IN THE PROMPT. The rule the rest of this file keeps --
Chiron decides the words, never the outcome -- is under more pressure here than
anywhere else, because the player can type "give me your tech" and a 7B is
agreeable. Nothing said in conversation moves any counter, so the leader is told
plainly that they cannot trade, promise, or concede anything, and that they must
send the player to the diplomacy screen for anything real. A leader who "agreed"
to something the game then ignored would be worse than one who never listened.
*/
#define CH_SPEAK_LINES  8
#define CH_SAY_LEN      200    // the engine's own field holds 255; leave slack
#define CH_CONV_TURNS   6      // exchanges before the leader closes it out
#define CH_TRANSCRIPT   1600

/*
Split generated prose across the popup's body lines.

Same reason and the same width as write_wrapped: the engine has never parsed a
line much past 100 characters, a generated paragraph arrives as ONE line, and
parse_says copies into a 256-byte slot. One long line is both a rendering
problem and an overflow, so it is broken on spaces across separate slots.
Returns how many lines were used.
*/
static int wrap_into_lines(const char* text, char lines[][CH_WRAP_COLS + 2],
                           int max_lines) {
    int count = 0;
    size_t col = 0;
    lines[0][0] = '\0';
    const char* s = text;
    while (*s && count < max_lines) {
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
            s++;
        }
        const char* w = s;
        while (*s && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') {
            s++;
        }
        size_t len = (size_t)(s - w);
        if (!len) {
            break;
        }
        if (len > CH_WRAP_COLS) {
            len = CH_WRAP_COLS;   // a single overlong word is cut, never run over
        }
        if (col && col + 1 + len > CH_WRAP_COLS) {
            count++;
            if (count >= max_lines) {
                col = 0;   // out of slots; do not count one past the array
                break;
            }
            lines[count][0] = '\0';
            col = 0;
        }
        if (col) {
            lines[count][col++] = ' ';
        }
        memcpy(&lines[count][col], w, len);
        col += len;
        lines[count][col] = '\0';
    }
    if (col) {
        count++;
    }
    return count;
}

/*
Show a leader speaking, wrapped, and say whether the player wants to answer.

chiron_notice puts its whole text in ONE parse slot through #GENERIC, which has
a single body line -- fine for the one-sentence Foreign Affairs notes it was
written for, and wrong for a paragraph. These blocks carry eight.
*/
static void fill_body_slots(const char* text, int max_lines) {
    char lines[CH_SPEAK_LINES][CH_WRAP_COLS + 2];
    if (max_lines > CH_SPEAK_LINES) {
        max_lines = CH_SPEAK_LINES;
    }
    int count = wrap_into_lines(text, lines, max_lines);
    for (int i = 0; i < max_lines; i++) {
        parse_says(i + 1, i < count ? lines[i] : "", -1, -1);
    }
}

static bool speak_and_offer(const char* caption, const char* text,
                            bool allow_reply) {
    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, caption, -1, -1);
    fill_body_slots(text, CH_SPEAK_LINES);
    int choice = X_pop_2("modmenu",
        allow_reply ? "CHIRONSPEAK" : "CHIRONSPOKE", 0);
    parse_state_restore(&saved);
    return allow_reply && choice == 1;
}

/*
Take a line of the player's own text. Returns false if they said nothing.

Flattened to a single line before it goes anywhere: the field is single-line
already, but the value reaches a prompt where a newline would let typed text
pose as one of our own section headings.
*/
static bool ask_player_line(const char* caption, char* out, size_t out_len) {
    char buf[CH_SAY_LEN + 8];
    buf[0] = '\0';

    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, caption, -1, -1);
    int ok = X_pop_ask_6("modmenu", "CHIRONSAY", CH_SAY_LEN, buf, 0, 0);
    parse_state_restore(&saved);
    if (!ok) {
        return false;
    }
    size_t n = 0;
    bool gap = false;
    for (const char* p = buf; *p && n + 1 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            gap = (n > 0);
            continue;
        }
        if (c < 0x20) {
            continue;
        }
        if (gap) {
            out[n++] = ' ';
            gap = false;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n > 0;
}

/*
Cut a leading "Santiago:" off a reply.

The transcript is a script -- one speaker label per line -- and the close of a
prompt is not the only thing a small model imitates: it copies the SHAPE of what
it was shown. Reproduced on the bridge, Santiago answered a demand for tech with
"Colonel Santiago: Flexibility is the attribute of the weak." That is invisible
to the existing filters, because is_scaffolding only cuts an ALL-CAPS label and
this is ordinary title case.

Matched against the specific names in play rather than "any short word before a
colon", so real speech survives -- "We swear it: the Pact of Brotherhood stands."
must not be touched.
*/
static void strip_speaker_label(char* text, const char* const* names, int count) {
    char* s = text;
    while (*s == ' ' || *s == '"') {
        s++;
    }
    for (int i = 0; i < count; i++) {
        if (!names[i] || !names[i][0]) {
            continue;
        }
        size_t n = strlen(names[i]);
        if (_strnicmp(s, names[i], n)) {
            continue;
        }
        const char* p = s + n;
        while (*p == ' ') {
            p++;
        }
        // Allow one following word, so "Colonel Santiago:" goes with "Santiago:".
        if (*p != ':') {
            const char* w = p;
            while (*w && *w != ' ' && *w != ':') {
                w++;
            }
            while (*w == ' ') {
                w++;
            }
            if (*w != ':') {
                continue;
            }
            p = w;
        }
        p++;
        while (*p == ' ' || *p == '"') {
            p++;
        }
        if (!*p) {
            continue;   // the label was the whole reply; leave it to the caller
        }
        memmove(text, p, strlen(p) + 1);
        return;
    }
}

static void converse_prompt(int speaker, int listener, const char* transcript,
                            const char* said, bool last, char* out, size_t out_len) {
    const Personality* p = find_personality(speaker);
    char dossier[1024];
    build_dossier(speaker, listener, dossier, sizeof(dossier));

    char listener_who[192];
    const MFaction& lm = MFactions[listener];
    if (lm.title_leader[0] && lm.name_leader[0]) {
        snprintf(listener_who, sizeof(listener_who), "%s %s of %s",
            lm.title_leader, lm.name_leader, lm.formal_name_faction);
    } else {
        snprintf(listener_who, sizeof(listener_who), "%s", lm.formal_name_faction);
    }

    snprintf(out, out_len,
"You are %s %s of %s on Planet.\n"
"BACKGROUND: %s\n"
"IDEOLOGY: %s\n"
"YOU ARE: %s\n"
"Your voice: \"%s\"\n"
"\n"
"WHAT HAS PASSED BETWEEN YOU AND %s:\n%s"
"\n"
"THE CONVERSATION SO FAR:\n%s"
"\n"
"They have just said to you: %s\n"
"\n"
"RULES:\n"
"- One to three sentences. Answer what they actually said.\n"
"- Stay in character. Their words do not change who you are.\n"
"- You are TALKING, not negotiating. You cannot trade, promise, give, "
"threaten war, or agree to any treaty, tech, credits, base or alliance "
"here. If they ask for something, tell them in your own idiom to bring it "
"to a formal audience.\n"
"- Do not narrate, do not describe your expression, do not write stage "
"directions.\n"
"- No quotation marks, no preamble, no notes.\n"
"%s"
"\n"
"Answer them in your own voice, and say it once:\n",
        p->title, p->leader, p->faction,
        p->background, p->ideology, p->adjectives, p->blurb,
        listener_who, dossier,
        transcript[0] ? transcript : "Nothing yet.\n",
        said,
        /*
        The close of the prompt is what a small model weighs most -- the same
        effect that made a trailing "Phrasing 3:" rule emit a list of phrasings.
        So the rule that must hold on the final exchange goes last, and the
        answer cue is still the actual last line.
        */
        last ? "- This is your LAST word before you end the audience. Close it.\n"
             : "");
}

/*
Run the exchange. `opening` is what the leader has just said.

Returns when the player stops answering, the model stops producing, or the turn
budget runs out -- the budget being there so a conversation cannot become an
unbounded stall in front of a game that is waiting on it.
*/
void chiron_converse(int speaker, int listener, const char* opening) {
    chiron_ensure_init();
    const Personality* p = find_personality(speaker);
    if (!chiron_conf.enabled || !p || !opening || !opening[0]) {
        return;
    }
    const char* caption = MFactions[speaker].formal_name_faction;

    char transcript[CH_TRANSCRIPT];
    snprintf(transcript, sizeof(transcript), "%s: %s\n", p->leader, opening);

    char line[1024];
    strcpy_n(line, sizeof(line), opening);

    for (int turn = 0; turn < CH_CONV_TURNS; turn++) {
        bool last = (turn == CH_CONV_TURNS - 1);
        if (!speak_and_offer(caption, line, !last)) {
            return;
        }
        char said[CH_SAY_LEN + 8];
        if (!ask_player_line(caption, said, sizeof(said))) {
            return;
        }

        static char prompt[6144];
        converse_prompt(speaker, listener, transcript, said,
            turn == CH_CONV_TURNS - 2, prompt, sizeof(prompt));

        static char reply[4096];
        if (!http_generate(prompt, reply, sizeof(reply), chiron_conf.max_tokens)) {
            ch_log("[converse] %s: no reply from the backend\n",
                MFactions[speaker].filename);
            speak_and_offer(caption,
                "\"...\" The commlink carries nothing but static.", false);
            return;
        }
        strip_preamble(reply);
        const char* labels[] = {
            p->leader, p->title, MFactions[speaker].name_leader,
            MFactions[speaker].title_leader, MFactions[listener].name_leader,
        };
        strip_speaker_label(reply, labels, 5);
        tidy_reply(reply);
        if (!reply[0]) {
            ch_log("[converse] %s: reply was empty after tidying\n",
                MFactions[speaker].filename);
            return;
        }

        /*
        Oldest exchanges fall off the front rather than the back: what they just
        said is what they must answer, and a transcript trimmed at the tail
        would drop exactly that.
        */
        size_t used = strlen(transcript);
        char add[CH_SAY_LEN + 1200];
        snprintf(add, sizeof(add), "%s: %s\n%s: %s\n",
            MFactions[listener].name_leader[0]
                ? MFactions[listener].name_leader : "They", said,
            p->leader, reply);
        size_t addlen = strlen(add);
        if (used + addlen + 1 > sizeof(transcript)) {
            size_t drop = used + addlen + 1 - sizeof(transcript);
            if (drop > used) {
                drop = used;
            }
            const char* cut = strchr(transcript + drop, '\n');
            cut = cut ? cut + 1 : transcript + used;
            memmove(transcript, cut, strlen(cut) + 1);
            used = strlen(transcript);
        }
        snprintf(transcript + used, sizeof(transcript) - used, "%s", add);

        strcpy_n(line, sizeof(line), reply);
        ch_log("[converse] %s exchange %d\n", MFactions[speaker].filename, turn + 1);
    }
    speak_and_offer(caption, line, false);
}

// ── probe protests ─────────────────────────────────────────────────────────

/*
Give the player something to say when an ally robs them.

The game exempts probe teams from diplomacy entirely. A faction you are not at
war with can strip your labs every few turns and the only answers are to accept
it or declare war, which usually costs more than the research did. There is no
way to object and have the objection mean anything.

Two things make it fixable cheaply. The engine already counts successful
operations per pair in diplo_stolen_techs and diplo_mind_control, so a theft is
detected by diffing those rather than by instrumenting probe()'s 600-line
dispatch. And veh_action.cpp already gates whether a probe unit acts on a
target, so refusing on a faction's behalf is one more condition on an if that
exists.

Note what the engine gate does and does not cover: it tests DIPLO_PACT and NOT
DIPLO_TREATY, so an AI pact partner already declines unless the probe was
waypointed onto the base, while treaty partners help themselves. The protest
applies to everyone regardless -- what changes with standing is what you can
threaten, since a pact is only leverage if there is a pact to withdraw.

Chiron decides the WORDS, never the OUTCOME. Whether they back down is settled
below from faction state on the same terms the rest of the engine uses; the
model is only asked to say it in character. A leader who agrees and then keeps
stealing would be worse than no feature at all.
*/
#define CH_WARN_DURATION 30   // turns a heeded warning holds

enum ChironStanding {
    CH_STAND_NONE = 0,
    CH_STAND_TREATY,
    CH_STAND_PACT,
};

static int standing_with(int a, int b) {
    if (a < 1 || b < 1 || a >= MaxPlayerNum || b >= MaxPlayerNum) {
        return CH_STAND_NONE;
    }
    if (Factions[a].diplo_status[b] & DIPLO_PACT) {
        return CH_STAND_PACT;
    }
    if (Factions[a].diplo_status[b] & DIPLO_TREATY) {
        return CH_STAND_TREATY;
    }
    return CH_STAND_NONE;
}

/*
True while faction_id is under a warning from tgt that it agreed to.

Stored on the WARNED faction, indexed by who warned them, as the turn it was
given; negative records a refusal, which binds nothing but is remembered. The
warning lapses after CH_WARN_DURATION so it is a reprieve rather than permanent
immunity -- and so a single early conversation cannot settle the whole game.
*/
bool chiron_probe_warned(int faction_id, int tgt_faction) {
    if (faction_id < 1 || faction_id >= MaxPlayerNum
        || tgt_faction < 1 || tgt_faction >= MaxPlayerNum) {
        return false;
    }
    int when = MFactions[faction_id].chiron_warned_turn[tgt_faction];
    if (when <= 0) {
        return false;
    }
    // A vendetta cancels every promise made before it.
    if (Factions[faction_id].diplo_status[tgt_faction] & DIPLO_VENDETTA) {
        return false;
    }
    return *CurrentTurn - when < CH_WARN_DURATION;
}

/*
Did faction_id refuse a warning from tgt_faction, recently enough to still be
grounds for war?

Refusal is stored as a negative turn on the faction that refused, indexed by
whoever warned them, so the subject of the question is the FIRST argument --
mirroring chiron_probe_warned above, and matching the call in double_cross(),
which passes the defender first because it is the defender's refusal that
excuses the attacker. The heading on this comment used to say the opposite.

It binds nothing by itself -- what it buys is the right to
break off relations without the usual dishonour, which double_cross() grants by
way of its own is_victim flag.

The same window as a heeded warning applies, so a refusal in 2120 does not
justify a betrayal in 2320. Grounds you never acted on go stale.
*/
bool chiron_probe_refused(int faction_id, int tgt_faction) {
    if (faction_id < 1 || faction_id >= MaxPlayerNum
        || tgt_faction < 1 || tgt_faction >= MaxPlayerNum) {
        return false;
    }
    int when = MFactions[faction_id].chiron_warned_turn[tgt_faction];
    if (when >= 0) {
        return false;
    }
    return *CurrentTurn - (-when) < CH_WARN_DURATION;
}

/*
Will they back down? Decided from faction state, never by the model.

The question is whether they have more to lose from your anger than from giving
up the operation, so it turns on what standing they would forfeit and whether
you are in any position to make it hurt. Ranking is the engine's own power
order.
*/
static bool will_heed_warning(int speaker, int listener) {
    int stand = standing_with(speaker, listener);
    int mine = Factions[listener].ranking;
    int theirs = Factions[speaker].ranking;

    // A pact is worth keeping unless they hold you in contempt.
    if (stand == CH_STAND_PACT) {
        return theirs <= mine + 3;
    }
    // A treaty is thinner; they weigh it against your strength.
    if (stand == CH_STAND_TREATY) {
        return theirs <= mine + 1;
    }
    // No standing to lose. Only raw strength talks, and rarely.
    return mine > theirs + 1;
}

static void protest_prompt(int speaker, int listener, bool heeds,
                           char* out, size_t out_len) {
    const Personality* p = find_personality(speaker);
    int stand = standing_with(speaker, listener);
    const char* bond =
        stand == CH_STAND_PACT   ? "You are bound to them by a PACT." :
        stand == CH_STAND_TREATY ? "You hold a TREATY of friendship with them." :
                                   "You have no treaty or pact with them.";
    /*
    Never name a bond in the branch where there is none. An earlier wording said
    they had "no formal tie to withdraw" and Santiago answered "our treaty with
    Gaia's Stepdaughters does not apply here" -- the word alone was enough to
    conjure the thing it was denying.
    */
    const char* leverage =
        stand == CH_STAND_PACT   ? "They have threatened to tear up the pact." :
        stand == CH_STAND_TREATY ? "They have threatened to tear up the treaty." :
                                   "Nothing binds the two of you. They have only "
                                   "their own strength to threaten you with.";

    char dossier[1024];
    build_dossier(speaker, listener, dossier, sizeof(dossier));

    // The person, not the letterhead -- same reason as build_prompt.
    char listener_who[192];
    const MFaction& lm = MFactions[listener];
    if (lm.title_leader[0] && lm.name_leader[0]) {
        snprintf(listener_who, sizeof(listener_who), "%s %s of %s",
            lm.title_leader, lm.name_leader, lm.formal_name_faction);
    } else {
        snprintf(listener_who, sizeof(listener_who), "%s", lm.formal_name_faction);
    }

    snprintf(out, out_len,
"You are %s %s of %s on Planet.\n"
"BACKGROUND: %s\n"
"IDEOLOGY: %s\n"
"YOU ARE: %s\n"
"Your voice: \"%s\"\n"
"\n"
"WHAT HAS PASSED BETWEEN YOU:\n%s"
"\n"
"THE SITUATION:\n"
"- Your probe teams have been caught stealing from %s.\n"
"- %s\n"
"- They have confronted you and demanded you call your probe teams off.\n"
"- %s\n"
"\n"
"You have decided to %s.\n"
"\n"
"Answer them. Conversation %d.\n"
"\n"
"RULES:\n"
"- Two or three sentences. Speak directly to them.\n"
"- %s\n"
"- Do not apologise for what you are. Stay in character.\n"
"- No quotation marks, no preamble, no notes.\n"
"\n"
"Say it once, and do not restate it in other words.\n"
"\n"
"YOUR ANSWER:\n",
        p->title, p->leader, p->faction,
        p->background, p->ideology, p->adjectives, p->blurb,
        dossier,
        listener_who,
        bond, leverage,
        heeds ? "CALL THEM OFF" : "REFUSE",
        variation_counter(),
        /*
        "Concede without grovelling" produced one flat sentence, and the same
        one from every leader -- "your concern is noted, my probe teams will
        withdraw". Conceding is the harder of the two to write, so it needs the
        more specific direction: give a reason of your own, in your own idiom.
        */
        heeds
            ? "You are agreeing to stop. Say so plainly and give your OWN "
              "reason for it, in your own idiom -- a decision you have taken, "
              "not a surrender. Do not use the words 'noted' or 'immediately'."
            : "You are refusing. Make plain that you will not be dictated to.");
}

/*
Run the confrontation. Returns true if they agreed to stop.

Called when the player has just discovered a theft and chosen to object.
*/
static bool run_protest(int speaker, int listener) {
    bool heeds = will_heed_warning(speaker, listener);
    const Personality* p = find_personality(speaker);

    char answer[1024];
    answer[0] = '\0';
    if (p) {
        static char prompt[4096];
        protest_prompt(speaker, listener, heeds, prompt, sizeof(prompt));
        static char reply[4096];
        if (http_generate(prompt, reply, sizeof(reply), chiron_conf.max_tokens)) {
            strip_preamble(reply);
            tidy_reply(reply);
            strcpy_n(answer, sizeof(answer), reply);
        }
    }
    if (!answer[0]) {
        // Bridge down or unusable reply: the outcome still stands.
        strcpy_n(answer, sizeof(answer), heeds
            ? "\"Very well. Our operatives will be recalled.\""
            : "\"We will do as we see fit. Do not presume to instruct us.\"");
    }

    MFactions[speaker].chiron_warned_turn[listener] =
        (int16_t)(heeds ? *CurrentTurn : -*CurrentTurn);
    ch_log("[protest] %s %s (standing=%d)\n", MFactions[speaker].filename,
        heeds ? "agreed to stop" : "refused", standing_with(speaker, listener));

    /*
    Their answer opens a conversation rather than closing one. The outcome is
    already settled above and nothing said here can move it -- which is exactly
    why it is safe to let the player argue with them about it.
    */
    chiron_converse(speaker, listener, answer);

    /*
    Say plainly what a refusal just bought, or the mechanic is invisible and the
    demand becomes the button that changes nothing this feature exists to avoid.
    */
    if (!heeds && standing_with(speaker, listener) != CH_STAND_NONE) {
        char notice[256];
        snprintf(notice, sizeof(notice),
            "They have refused us. For the next %d turns we may break off "
            "relations with them without dishonour.", CH_WARN_DURATION);
        chiron_notice("Foreign Affairs", notice);
    }
    return heeds;
}

/*
The same conversation with the sides swapped: they caught YOUR probe teams.

Everything above answers the case where an AI robs the player. The mirror image
was missing entirely, and from the seat that is the more visible half -- the
dossier line at build_dossier() puts "their probe teams have stolen your
research" into every prompt the moment you steal from someone, so leaders start
raising it in ordinary conversation while nothing whatever follows from it. A
grievance a faction states and can never act on is worse than one it never
mentions, because the player can hear that the game has noticed and watch it do
nothing.

The asymmetry that remains is deliberate. An AI's answer is decided by
will_heed_warning(); the player's is a choice, so there is nothing to predict
and the model is only asked to make the demand. What binds afterwards is the
same field in both directions, so the machinery below this is shared.
*/
static void demand_prompt(int speaker, int listener, char* out, size_t out_len) {
    const Personality* p = find_personality(speaker);
    int stand = standing_with(speaker, listener);
    const char* bond =
        stand == CH_STAND_PACT   ? "You are bound to them by a PACT, which makes "
                                   "this a betrayal and not merely a theft." :
        stand == CH_STAND_TREATY ? "You hold a TREATY of friendship with them, "
                                   "which they have abused." :
                                   "Nothing binds the two of you, so you have no "
                                   "agreement for them to have broken -- only your "
                                   "own strength to answer with.";
    /*
    Same rule as protest_prompt: never name a bond in the branch where there is
    none, or the word alone conjures the thing it is denying.
    */
    const char* leverage =
        stand == CH_STAND_NONE
            ? "Do not speak of treaties or pacts. There are none."
            : "You may hold what stands between you over them.";

    char dossier[1024];
    build_dossier(speaker, listener, dossier, sizeof(dossier));

    char listener_who[192];
    const MFaction& lm = MFactions[listener];
    if (lm.title_leader[0] && lm.name_leader[0]) {
        snprintf(listener_who, sizeof(listener_who), "%s %s of %s",
            lm.title_leader, lm.name_leader, lm.formal_name_faction);
    } else {
        snprintf(listener_who, sizeof(listener_who), "%s", lm.formal_name_faction);
    }

    snprintf(out, out_len,
"You are %s %s of %s on Planet.\n"
"BACKGROUND: %s\n"
"IDEOLOGY: %s\n"
"YOU ARE: %s\n"
"Your voice: \"%s\"\n"
"\n"
"WHAT HAS PASSED BETWEEN YOU:\n%s"
"\n"
"THE SITUATION:\n"
"- %s has been running probe teams against you.\n"
"- Your own operatives have caught them at it. There is no doubt.\n"
"- %s\n"
"- You are about to demand that they call their probe teams off.\n"
"\n"
"Speak to them. Conversation %d.\n"
"\n"
"RULES:\n"
"- Two or three sentences. Speak directly to them.\n"
"- Make the demand plainly. Do not ask a question you would accept 'no' to.\n"
"- %s\n"
"- Do not say what you will do if they refuse. You have not decided.\n"
"- No quotation marks, no preamble, no notes.\n"
"\n"
"Say it once, and do not restate it in other words.\n"
"\n"
"YOUR DEMAND:\n",
        p->title, p->leader, p->faction,
        p->background, p->ideology, p->adjectives, p->blurb,
        dossier,
        listener_who,
        bond,
        variation_counter(),
        leverage);
}

/*
They demand you stop. Returns true if the player agreed.

The player's word binds exactly as an AI's does -- chiron_warned_turn is written
the same way and read by the same gate in veh_action.cpp -- because a promise
only the AI can be held to is not a mechanic, it is a courtesy.
*/
static bool run_demand(int speaker, int listener) {
    const Personality* p = find_personality(speaker);

    char demand[1024];
    demand[0] = '\0';
    if (p) {
        static char prompt[4096];
        demand_prompt(speaker, listener, prompt, sizeof(prompt));
        static char reply[4096];
        if (http_generate(prompt, reply, sizeof(reply), chiron_conf.max_tokens)) {
            strip_preamble(reply);
            tidy_reply(reply);
            strcpy_n(demand, sizeof(demand), reply);
        }
    }
    if (!demand[0]) {
        strcpy_n(demand, sizeof(demand),
            "\"Your probe teams have been caught in our territory. Withdraw "
            "them, or we will treat the next one as an act of war.\"");
    }

    /*
    One popup, not two: their words are the body and the answer is the option
    list, so the demand and the reply to it are the same moment. The affirmative
    is SECOND because X_pop_2 returns the option index and the block reads true
    only on 1. Wrapped across the block's body lines rather than handed over as
    one string, because a generated paragraph is both wider than anything the
    engine renders and longer than a 256-byte parse slot.

    Third option: answer them in your own words first. The protest already ends
    in a conversation and this did not, which left the one moment a leader
    addresses the player DIRECTLY as the one moment they could not reply -- two
    buttons written by someone else, which is the thing that reading a fresh
    line is supposed to stop feeling like. Talking settles nothing on its own,
    so afterwards the same choice is put again through a two-option block;
    offered once, because a demand you can defer indefinitely is not a demand.
    */
    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, MFactions[speaker].formal_name_faction, -1, -1);
    fill_body_slots(demand, CH_SPEAK_LINES);
    int choice = X_pop_2("modmenu", "CHIRONDEMAND", 0);
    parse_state_restore(&saved);

    if (choice == 2) {
        chiron_converse(speaker, listener, demand);
        parse_state_save(&saved);
        parse_says(0, MFactions[speaker].formal_name_faction, -1, -1);
        fill_body_slots("They are waiting on your answer.", CH_SPEAK_LINES);
        choice = X_pop_2("modmenu", "CHIRONDEMAND2", 0);
        parse_state_restore(&saved);
    }
    bool agree = (choice == 1);

    MFactions[listener].chiron_warned_turn[speaker] =
        (int16_t)(agree ? *CurrentTurn : -*CurrentTurn);
    ch_log("[demand] %s demanded we stop; we %s (standing=%d)\n",
        MFactions[speaker].filename, agree ? "agreed" : "refused",
        standing_with(speaker, listener));

    /*
    Both answers cost something, and both have to be said out loud for the same
    reason the refusal notice exists on the other side: an option whose
    consequence is invisible is an option that reads as decoration.
    */
    char notice[320];
    if (agree) {
        snprintf(notice, sizeof(notice),
            "We have given our word. Our probe teams will not operate against "
            "the %s for %d turns. Breaking it will be remembered.",
            MFactions[speaker].adj_name_faction, CH_WARN_DURATION);
    } else {
        snprintf(notice, sizeof(notice),
            "We have refused them. For the next %d turns they may break off "
            "relations with us without dishonour.", CH_WARN_DURATION);
    }
    chiron_notice("Foreign Affairs", notice);
    return agree;
}

/*
The player is about to break a promise they gave. Returns true if they meant to.

Called from the probe gate. Breaking your word has to be possible or the promise
is a lock rather than a decision, and it has to be recorded or it is free. The
engine already keeps the pair of counters for exactly this -- diplo_wrongs on
the one who broke faith, diplo_betrayed on the one who was owed -- and
build_dossier reads both, so the leader raises it themselves next time you
speak. No new bookkeeping, and no new rule.
*/
bool chiron_confirm_break_word(int breaker, int tgt) {
    chiron_ensure_init();

    syn_parse_state_t saved;
    parse_state_save(&saved);
    char body[320];
    snprintf(body, sizeof(body),
        "We gave the %s our word that our probe teams would stand down, "
        "and that word still holds. Proceeding will be taken as a betrayal.",
        MFactions[tgt].adj_name_faction);
    parse_says(0, "Operations Director", -1, -1);
    fill_body_slots(body, CH_SPEAK_LINES);
    bool proceed = X_pop_2("modmenu", "CHIRONBREAKWORD", 0);
    parse_state_restore(&saved);

    if (!proceed) {
        return false;
    }
    MFactions[breaker].chiron_warned_turn[tgt] = 0;
    Factions[breaker].diplo_wrongs[tgt]++;
    Factions[tgt].diplo_betrayed[breaker]++;
    ch_log("[demand] we broke our word to %s\n", MFactions[tgt].filename);
    return true;
}

/*
Confront them over a probe team you caught in the act. Returns true if they
agreed to stop.

The counters this feature otherwise watches only move on a COMPLETED operation
(probe.cpp:1233, inside `case PRB_PROCURE_RESEARCH_DATA`), so intercepting a
probe team before it acts leaves nothing to detect -- which is the case a player
is most likely to think ought to open the conversation, because it is the one
where they have the proof in hand. veh_action.cpp calls this from the capture
path, where there is no theft to diff and the evidence is the unit itself.

No vendetta check here: the interception path only runs under TRUCE or TREATY.
*/
bool chiron_probe_confront(int speaker, int listener) {
    chiron_ensure_init();
    if (!chiron_conf.enabled || !chiron_conf.probe_protests
        || !find_personality(speaker)) {
        return false;
    }
    return run_protest(speaker, listener);
}

/*
Should the capture popup carry Chiron's fourth option?

Split out so veh_action.cpp can choose the block without reaching into
chiron_conf, and so the answer is the same one chiron_probe_confront() will act
on -- an option that leads nowhere is worse than no option.
*/
bool chiron_can_confront(int speaker) {
    chiron_ensure_init();
    return chiron_conf.enabled && chiron_conf.probe_protests
        && find_personality(speaker) != NULL;
}

/*
Offer to raise their probe teams with them, at a moment of the player's choosing.

The turn-start popup is the only other way in, and it is a one-shot: say nothing
and the grievance is gone until they rob you again. That is the wrong shape for
the one feature here that is supposed to be a conversation, so the same
confrontation is offered at the top of diplomacy whenever there is something
unresolved to raise.

Self-limiting by construction -- raising it writes chiron_warned_turn either way,
and the guard below reads it -- so it cannot become a prompt on every visit.
Declining is remembered only for the turn, because declining to bring it up now
is not the same as deciding to let it go.
*/
static int raise_offered_turn[MaxPlayerNum];

bool chiron_probe_grievance(int player_id, int ai_id) {
    chiron_ensure_init();
    if (!chiron_conf.enabled || !chiron_conf.probe_protests) {
        return false;
    }
    if (player_id < 1 || player_id >= MaxPlayerNum
        || ai_id < 1 || ai_id >= MaxPlayerNum || player_id == ai_id) {
        return false;
    }
    if (Factions[player_id].diplo_stolen_techs[ai_id]
        + Factions[player_id].diplo_mind_control[ai_id] <= 0) {
        return false;   // they have never robbed us
    }
    if (Factions[player_id].diplo_status[ai_id] & DIPLO_VENDETTA) {
        return false;   // nothing left to threaten
    }
    // Already settled one way or the other, and not yet lapsed.
    return !chiron_probe_warned(ai_id, player_id)
        && !chiron_probe_refused(ai_id, player_id);
}

void chiron_offer_raise(int player_id, int ai_id) {
    if (!chiron_probe_grievance(player_id, ai_id)) {
        return;
    }
    if (raise_offered_turn[ai_id] == *CurrentTurn + 1) {
        return;   // asked already this turn; +1 so turn 0 is not "asked"
    }
    raise_offered_turn[ai_id] = *CurrentTurn + 1;

    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, MFactions[ai_id].formal_name_faction, -1, -1);
    fill_body_slots("Their probe teams have operated against us and the matter "
                    "is still open. Raise it with them now?", CH_SPEAK_LINES);
    bool raise = X_pop_2("modmenu", "CHIRONRAISE", 0);
    parse_state_restore(&saved);

    if (raise) {
        run_protest(ai_id, player_id);
    }
}

/*
Watch for probe operations either way and open the conversation about them.

Diffed rather than hooked, for the same reason Planetnet is: the counters are
already maintained by the engine and cost nothing to read, where probe()'s
dispatch is 600 lines with a dozen action paths.

Both directions are watched from here. Factions[victim].diplo_stolen_techs[thief]
is the shape of the field (probe.cpp:1233 does tgt->diplo_stolen_techs[veh_fc_id]++),
so the player's row counts what was done TO them and every other faction's row,
read at the player's column, counts what the player did to it.

THE BASELINE IS PER GAME, NOT PER FACTION. It used to be `!prev`, per pair, which
is not a baseline at all: a faction going 0 -> 1 has prev == 0, so the very first
theft each faction ever committed was swallowed and only quietly raised the
baseline to 1. The feature therefore never fired until a faction robbed you
TWICE, which from the seat is indistinguishable from it never firing. What the
guard was actually for -- counters loaded out of a save reading as a screenful of
fresh outrages -- needs one flag for the whole game, taken once.
*/
static int  theft_seen[MaxPlayerNum];     // thefts against the player, by faction
static int  theft_done[MaxPlayerNum];     // thefts by the player, against faction
static int  theft_seen_turn = -1;
static bool theft_baselined = false;

static void theft_totals(int faction_id, int i, int* against_us, int* by_us) {
    *against_us = Factions[faction_id].diplo_stolen_techs[i]
                + Factions[faction_id].diplo_mind_control[i];
    *by_us      = Factions[i].diplo_stolen_techs[faction_id]
                + Factions[i].diplo_mind_control[faction_id];
}

void chiron_check_thefts(int faction_id) {
    chiron_ensure_init();
    if (!chiron_conf.enabled || !chiron_conf.probe_protests) {
        return;
    }
    // Only the player is party to this; the AI factions settle it among themselves.
    if (faction_id < 1 || faction_id != (MapWin ? MapWin->cOwner : 0)) {
        return;
    }
    /*
    A turn going backwards is a different game -- a new one started without
    quitting, or a save loaded from earlier in this one. Either way the totals we
    remember belong to a game that is no longer being played, so re-baseline
    rather than report the difference between two histories.
    */
    if (*CurrentTurn < theft_seen_turn) {
        theft_baselined = false;
    }
    theft_seen_turn = *CurrentTurn;

    if (!theft_baselined) {
        for (int i = 1; i < MaxPlayerNum; i++) {
            theft_totals(faction_id, i, &theft_seen[i], &theft_done[i]);
        }
        theft_baselined = true;
        ch_log("[protest] baseline taken at turn %d\n", *CurrentTurn);
        return;
    }

    for (int i = 1; i < MaxPlayerNum; i++) {
        if (i == faction_id) {
            continue;
        }
        int total, mine;
        theft_totals(faction_id, i, &total, &mine);
        int prev = theft_seen[i];
        int prev_mine = theft_done[i];
        theft_seen[i] = total;
        theft_done[i] = mine;

        if (Factions[faction_id].diplo_status[i] & DIPLO_VENDETTA) {
            continue;   // already at war; there is nothing left to threaten
        }
        /*
        They caught us. Checked first because if both happened in the same turn
        the demand made of the player is the one they cannot postpone -- our own
        grievance keeps, and chiron_offer_raise() will bring it up in diplomacy.
        */
        if (mine > prev_mine && !chiron_probe_warned(faction_id, i)
            && !chiron_probe_refused(faction_id, i) && find_personality(i)) {
            run_demand(i, faction_id);
        }
        if (total <= prev) {
            continue;
        }
        if (chiron_probe_warned(i, faction_id)) {
            continue;   // they are already under a warning they accepted
        }

        syn_parse_state_t saved;
        parse_state_save(&saved);
        parse_says(0, MFactions[i].formal_name_faction, -1, -1);
        int stand = standing_with(i, faction_id);
        parse_says(1, stand == CH_STAND_PACT
            ? "Their probe teams have been caught operating against us, in "
              "defiance of our pact. Demand that they stop?"
            : stand == CH_STAND_TREATY
            ? "Their probe teams have been caught operating against us, in "
              "defiance of our treaty. Demand that they stop?"
            : "Their probe teams have been caught operating against us. "
              "Demand that they stop?", -1, -1);
        /*
        X_pop_2, because bare X_pop looks in the WRONG FILE.

        gui_dialog.cpp:26 -- `X_pop(label, fn)` forwards to
        `X_pop_9(ScriptFile, ...)`, and ScriptFile is Script.txt. Every stock
        caller relies on that: #VERYLARGEMAP, #TIMELIMIT and #RETIREWARNING are
        all Script.txt labels. Ours are not. #CHIRONPROBE lives in modmenu.txt,
        so the lookup was for a label that file has never contained, and the
        confrontation could not have appeared even once -- there is no error
        for it either, which is why it read as "the feature never triggers".
        */
        bool demand = X_pop_2("modmenu", "CHIRONPROBE", 0);
        parse_state_restore(&saved);
        if (demand) {
            run_protest(i, faction_id);
        }
    }
}

// ── planetary news digest ──────────────────────────────────────────────────

/*
An in-fiction bulletin on the state of Planet, on demand at Alt+N.

Unlike everything else here this is not a rewrite of shipped text -- there is no
vanilla equivalent to fall back to, so it is deliberately the one feature the
player asks for rather than one that happens to them. That also settles the cost
question: a couple of seconds is a wait you chose, where the same pause during
turn processing would read as a hang.

The facts are gathered from engine state and handed over as a list; the model's
only job is to put them in the mouth of a news service. Nothing here is invented
by the model that the player could act on -- if it embellishes, it embellishes
the prose around numbers that are already true.
*/
#define CH_NEWS_LINES 8

/*
NEWS IS WHAT CHANGED, and a snapshot is not news.

Handed the current standings, the model transcribed the table and then padded to
the token ceiling with invention -- "Spartan Federation suffers losses", "Gaia's
Stepdaughters face food shortages", and a war between two factions who were not
at war. That is not the model being unruly. A snapshot contains no events, so a
prompt that asks for a report of events leaves it nothing to write and every
incentive to make some up.

Given the same turn expressed as deltas it reports accurately and invents
nothing. So we keep last bulletin's numbers and diff against them.
*/
struct NewsSnapshot {
    bool valid;
    int  turn;
    bool alive[MaxPlayerNum];
    int  base_count[MaxPlayerNum];
    int  major_atrocities[MaxPlayerNum];
    int  diplo_status[MaxPlayerNum][MaxPlayerNum];
};

static NewsSnapshot last_news = {};

static void take_snapshot(NewsSnapshot& s) {
    s.valid = true;
    s.turn = *CurrentTurn;
    for (int i = 1; i < MaxPlayerNum; i++) {
        s.alive[i] = is_alive(i);
        s.base_count[i] = Factions[i].base_count;
        s.major_atrocities[i] = Factions[i].major_atrocities;
        for (int j = 1; j < MaxPlayerNum; j++) {
            s.diplo_status[i][j] = Factions[i].diplo_status[j];
        }
    }
}

// The standings line that closes every bulletin: who leads, and where we stand.
static void news_standing(char* out, size_t out_len) {
    int me = MapWin ? MapWin->cOwner : 0;
    int lead = 0;
    for (int i = 1; i < MaxPlayerNum; i++) {
        if (is_alive(i) && (!lead || Factions[i].base_count > Factions[lead].base_count)) {
            lead = i;
        }
    }
    if (!lead) {
        out[0] = '\0';
        return;
    }
    if (me >= 1 && me < MaxPlayerNum && me != lead) {
        snprintf(out, out_len, "Standing now: %s leads with %d bases; we hold %d.\n",
            MFactions[lead].formal_name_faction, Factions[lead].base_count,
            Factions[me].base_count);
    } else {
        snprintf(out, out_len, "Standing now: we lead with %d bases.\n",
            Factions[lead].base_count);
    }
}

/*
Everything that moved since the last bulletin. Returns how many things did --
zero means there is no dispatch to write, and the caller must not ask for one.
*/
static int news_deltas(const NewsSnapshot& prev, char* out, size_t out_len) {
    int changes = 0;
    size_t n = 0;
    out[0] = '\0';
    #define FACT(...) \
        n = strlen(out); \
        snprintf(out + n, n < out_len ? out_len - n : 0, __VA_ARGS__); \
        changes++

    for (int i = 1; i < MaxPlayerNum; i++) {
        if (!prev.alive[i] && !is_alive(i)) {
            continue;
        }
        if (prev.alive[i] && !is_alive(i)) {
            FACT("- %s has been eliminated.\n", MFactions[i].formal_name_faction);
            continue;
        }
        int d = Factions[i].base_count - prev.base_count[i];
        if (d > 0) {
            FACT("- %s founded or took %d base%s, and now holds %d.\n",
                MFactions[i].formal_name_faction, d, d > 1 ? "s" : "",
                Factions[i].base_count);
        } else if (d < 0) {
            FACT("- %s lost %d base%s, and now holds %d.\n",
                MFactions[i].formal_name_faction, -d, d < -1 ? "s" : "",
                Factions[i].base_count);
        }
        if (Factions[i].major_atrocities > prev.major_atrocities[i]) {
            FACT("- %s was condemned for a major atrocity.\n",
                MFactions[i].formal_name_faction);
        }

        // Treaties are mutual; report each pair once, from the lower index.
        for (int j = i + 1; j < MaxPlayerNum; j++) {
            if (!is_alive(j)) {
                continue;
            }
            int was = prev.diplo_status[i][j];
            int now = Factions[i].diplo_status[j];
            if (!(was & DIPLO_VENDETTA) && (now & DIPLO_VENDETTA)) {
                FACT("- %s declared war on %s.\n",
                    MFactions[i].formal_name_faction,
                    MFactions[j].formal_name_faction);
            } else if ((was & DIPLO_VENDETTA) && !(now & DIPLO_VENDETTA)) {
                FACT("- %s and %s have stopped fighting.\n",
                    MFactions[i].formal_name_faction,
                    MFactions[j].formal_name_faction);
            }
            if (!(was & DIPLO_PACT) && (now & DIPLO_PACT)) {
                FACT("- %s and %s signed a pact.\n",
                    MFactions[i].formal_name_faction,
                    MFactions[j].formal_name_faction);
            }
        }
    }
    #undef FACT
    return changes;
}

static void news_notice(const char* text) {
    chiron_notice("Planetnet", text);
}

/*
Every number in a dispatch has to be a number we handed over.

The deltas rewrite above stopped the model inventing EVENTS. It did not stop it
inventing FIGURES, and a figure is the more dangerous of the two because it
looks like the kind of thing a wire service would know. A bulletin whose only
facts were base counts came back with "a 24-hour population of 76,879, with 51%
under agri, 32% industrial, and 17% urban" -- no population is ever passed to
the model, and the rule forbidding percentages was already in the prompt, last,
on its own line. It read the rule and wrote the percentages anyway.

So it is checked rather than asked for. Digits are compared as whole tokens, not
as substrings: "1" must not be satisfied by the "19" in the standings. Commas
inside a number are dropped on both sides so 76,879 and 76879 are one token.

A percent sign is refused outright. Nothing we pass is a share of anything, so
there is no percentage a truthful dispatch could contain.
*/
static bool number_in(const char* facts, const char* num, size_t num_len) {
    for (const char* p = facts; *p; ) {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        // Walk one whole number, skipping the commas inside it.
        size_t i = 0;
        bool same = true;
        const char* q = p;
        while (*q && (isdigit((unsigned char)*q) || (*q == ',' && isdigit((unsigned char)q[1])))) {
            if (*q != ',') {
                if (i >= num_len || *q != num[i]) {
                    same = false;
                }
                i++;
            }
            q++;
        }
        if (same && i == num_len) {
            return true;
        }
        p = q;
    }
    return false;
}

static bool news_numbers_ok(const char* facts, const char* reply) {
    if (strchr(reply, '%')) {
        ch_log("[news] rejected: percentage\n");
        return false;
    }
    for (const char* p = reply; *p; ) {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        char num[32];
        size_t n = 0;
        const char* q = p;
        while (*q && (isdigit((unsigned char)*q) || (*q == ',' && isdigit((unsigned char)q[1])))) {
            if (*q != ',' && n + 1 < sizeof(num)) {
                num[n++] = *q;
            }
            q++;
        }
        num[n] = '\0';
        if (n && !number_in(facts, num, n)) {
            ch_log("[news] rejected: %s is not one of ours\n", num);
            return false;
        }
        p = q;
    }
    return true;
}

/*
End on a full stop, wherever the last one is.

Two different cuts leave a dangling fragment behind, and the fix is the same for
both. The token ceiling ends the reply mid-word -- the bulletin in the
screenshot stopped on "providing sustenance and". And cutting a dateline out of
the middle leaves whatever led into it: "... holding steady. Cassandra
Directorate" is what survives once "Planetnet Bureau" and the second copy after
it are gone, because the faction name sits in front of the marker rather than
behind it.

tidy_reply does this already, as step 2 of three, but the other two steps are
diplomacy's: it caps at CH_MAX_SENTENCES and wraps the result in the pair of
quotes every shipped speech block has. A wire dispatch is neither spoken nor
four sentences by rule, so it gets the one step it needs rather than the whole
function.

A reply with no terminator at all is left alone, on tidy_reply's reasoning: text
with no full stop still beats an empty box.
*/
static void news_end_at_sentence(char* text) {
    size_t last = 0;
    for (size_t i = 0; text[i]; i++) {
        if ((text[i] == '.' || text[i] == '!' || text[i] == '?')
            && (text[i+1] == '\0' || text[i+1] == ' ' || text[i+1] == '\n'
                || text[i+1] == '\r' || text[i+1] == '\t')) {
            last = i + 1;
        }
    }
    if (last) {
        text[last] = '\0';
    }
}

/*
What to show when the model cannot be trusted with the facts.

There is no vanilla dispatch to fall back to -- this feature has no shipped
equivalent -- so the fallback is the facts themselves, flattened out of the
bullet list they arrive in. It reads plainly rather than well, which is the
correct trade: a dry true bulletin beats a fluent invented one, and the box is
never empty.
*/
static void news_plain(const char* facts, char* out, size_t out_len) {
    size_t j = 0;
    bool gap = true;   // Swallows leading space, and collapses runs of it.
    for (const char* p = facts; *p && j + 1 < out_len; p++) {
        char c = *p;
        if (c == '\n' || c == '\t' || c == ' ') {
            gap = true;
            continue;
        }
        // The "- " that opens each delta line has no meaning once flattened.
        if (c == '-' && gap && (p[1] == ' ' || p[1] == '\0')) {
            continue;
        }
        if (gap && j) {
            out[j++] = ' ';
        }
        gap = false;
        if (j + 1 < out_len) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

void chiron_show_news() {
    chiron_ensure_init();

    char facts[2048];
    char standing[256];
    news_standing(standing, sizeof(standing));

    if (!last_news.valid) {
        // Nothing to diff against yet, so the first bulletin is the standings.
        snprintf(facts, sizeof(facts),
            "This is the first bulletin of the colony.\n%s", standing);
    } else {
        char deltas[1536];
        int changes = news_deltas(last_news, deltas, sizeof(deltas));
        /*
        No news is not a prompt. Asked to report a turn in which nothing moved,
        the model filled the space with invention -- a base count that was never
        true, a growth rate nobody measured. Say so directly instead: it is
        honest, it is instant, and it costs no generation.
        */
        if (!changes) {
            take_snapshot(last_news);
            ch_log("[news] no developments since turn %d\n", last_news.turn);
            news_notice("No developments since the last bulletin.");
            return;
        }
        /*
        Hand over the elapsed span, not the two turn numbers.

        Given "turn 288, now turn 301" the model works out the gap itself and
        gets it wrong about one time in five -- "in the last 12 turns" for a
        thirteen turn gap. It is the same lesson as the deltas themselves: give
        it the fact rather than the means to derive it, because anything it
        derives it can derive incorrectly, and a wire service being confidently
        wrong about dates reads worse than one saying nothing.
        */
        int elapsed = *CurrentTurn - last_news.turn;
        if (elapsed < 1) {
            elapsed = 1;
        }
        snprintf(facts, sizeof(facts),
            "Since the last bulletin, %d turn%s ago:\n%s%s",
            elapsed, elapsed == 1 ? "" : "s", deltas, standing);
    }
    take_snapshot(last_news);

    int me = MapWin ? MapWin->cOwner : 0;
    const char* our_name = (me >= 1 && me < MaxPlayerNum)
        ? MFactions[me].formal_name_faction : "the colonists";

    static char prompt[4096];
    snprintf(prompt, sizeof(prompt),
/*
Name the two things separately and say which is which.

"You write for Planetnet, the wire service read across Planet" put two proper
nouns one clause apart that differ by three letters, and the model collapsed
them: "Nineteen bases now stand on Planetnet". The world has to be named on its
own line as the world before the service is named at all.
*/
"The world is called Planet. Planetnet is a wire service that reports on it; "
"Planetnet is not a place and nothing stands on it.\n"
"You write for Planetnet. Your readers are the colonists of %s.\n"
"\n"
"WHAT CHANGED:\n%s"
"\n"
"Dispatch %d. Report it for your readers.\n"
"\n"
"RULES:\n"
"- Three or four sentences of prose. Never a list.\n"
"- Lead with whatever matters most to us, and say what it means for us.\n"
"- No line longer than %d characters.\n"
"- Dry and clipped, the way a wire service writes on a frontier world.\n"
"- Start with the news itself: no headline, no byline, no date, no faction name "
"and colon at the front.\n"
"- Write the dispatch once. No byline, no bureau line and no sign-off after it, "
"and never a second version of the same news.\n"
"\n"
/*
The anti-invention rule goes last, on its own, for the same reason the
placeholder reminder does: the close of the prompt is what carries.

Naming the invented categories is worth the words. "Do not turn them into a
percentage" was already here and was ignored; what came back was a population
and a three-way split of it, neither of which is a number this prompt has ever
contained. It is enforced in news_numbers_ok either way -- this only saves the
round trip.
*/
"Report only what is listed above. Invent no battle, no famine, no treaty and "
"no faction beyond it. Every number you write must be one of the numbers above: "
"you do not know the population, you do not know how anyone makes a living, and "
"you cannot add, total, compare or turn any figure into a percentage or a "
"share.\n"
"\n"
"DISPATCH:\n",
        our_name, facts, variation_counter(), CH_WRAP_COLS);

    static char reply[4096];
    chiron_trace("news: prompt built (%d bytes)\n", (int)strlen(prompt));
    /*
    Two attempts, then the plain facts.

    A rejected dispatch is worth one retry because the failure is a sampling
    accident rather than a standing refusal -- the same prompt usually comes
    back clean. It is not worth two: the game blocks for the whole generation,
    and a third round trip turns a pause the player chose into one they notice.
    */
    bool have = false;
    for (int attempt = 0; attempt < 2 && !have; attempt++) {
        /*
        120 tokens is four sentences with no room left over. At 220 the model
        filled the slack rather than stopping, and what it filled it with was
        invented.
        */
        if (!http_generate(prompt, reply, sizeof(reply), 120)) {
            ch_log("[news] generation failed\n");
            news_notice("The relay is silent. No dispatch this turn.");
            return;
        }
        strip_preamble(reply);
        cut_at_meta(reply);
        cut_at_scaffolding(reply);
        // Before the number check, not after: cutting a dateline can take the
        // invented figures with it, and then there is no retry to spend.
        news_end_at_sentence(reply);
        have = news_numbers_ok(facts, reply);
    }
    if (!have) {
        ch_log("[news] falling back to the plain facts\n");
        news_plain(facts, reply, sizeof(reply));
    }

    /*
    Same width as the diplomacy wrapper, and for the same reason: the engine has
    never parsed a line longer than about 106 characters, and a generated
    paragraph arrives as one long line. See write_wrapped for the crash that
    taught us. Breaks are on spaces only, and a single word longer than the
    width is truncated rather than allowed to run over the line buffer.
    */
    char lines[CH_NEWS_LINES][CH_WRAP_COLS + 2];
    int count = 0;
    size_t col = 0;
    lines[0][0] = '\0';

    const char* s = reply;
    while (*s && count < CH_NEWS_LINES) {
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
            s++;
        }
        const char* w = s;
        while (*s && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') {
            s++;
        }
        size_t len = (size_t)(s - w);
        if (!len) {
            break;
        }
        if (len > CH_WRAP_COLS) {
            len = CH_WRAP_COLS;
        }
        if (col && col + 1 + len > CH_WRAP_COLS) {
            count++;
            if (count >= CH_NEWS_LINES) {
                // Out of lines. Clear col so the tail below cannot count a
                // ninth line into an array that holds eight.
                col = 0;
                break;
            }
            lines[count][0] = '\0';
            col = 0;
        }
        if (col) {
            lines[count][col++] = ' ';
        }
        memcpy(&lines[count][col], w, len);
        col += len;
        lines[count][col] = '\0';
    }
    if (col) {
        count++;
    }
    if (!count) {
        ch_log("[news] empty dispatch\n");
        return;
    }

    syn_parse_state_t saved;
    parse_state_save(&saved);
    parse_says(0, "Planetnet", -1, -1);
    for (int i = 0; i < CH_NEWS_LINES; i++) {
        parse_says(i + 1, i < count ? lines[i] : "", -1, -1);
    }
    ch_log("[news] dispatch, %d lines\n", count);
    popp("modmenu", "CHIRONNEWS", 0, 0, 0);
    parse_state_restore(&saved);
}

// ── Future Society repricing ───────────────────────────────────────────────

/*
The one part of the mod that changes the RULES, and the only one you can throw
mid-game and watch land.

Count the pips in the stock #SOCIO table and every Future Society is +6 gross
where nothing else clears +4. That row is not three options balanced against
each other, it is a row strictly better than the row above it -- not a choice,
a reward for reaching the tech.

But the pip count understates it, because TWO OF THE THREE PENALTIES ARE NEVER
PAID. social_calc() (faction.cpp:1191) zeroes EVERY negative on a Future
Society model when the faction holds one project:

    Cybernetic      + Network Backbone  -> negatives become 0
    Thought Control + Cloning Vats      -> negatives become 0
    Eudaimonic                          -> no clause; it always pays

So Cybernetic and Thought Control are +6/0 for whoever lands the project. That
inverts the obvious reading: Eudaimonic's +6/-2 looks like the outlier and is
in fact the only one of the three whose cost is real, which is a reason to
price it gently rather than to punish it. It gets ONE penalty here; the other
two get two, and keep their project escape exactly as before.

The clause zeroes any negative, not a nominated one, so moving a penalty from
POLICE to GROWTH does not make it unbuyable. It changes what you lose until
the project lands, and a project is singular -- for everyone who never builds
it, that is the whole game.

Modding this needs no code because the ROWS are data: social_calc reads
SocialField[cat].soc_effect[model] on every recalculation, and Thinker's own
mod_social_ai evaluates the resulting effect vector rather than the name, so
the AI adapts to a re-cut row for free. The COLUMNS are the opposite -- eleven
hardcoded meanings the engine reasons about directly -- which is why this
changes which model grants what, and never what an effect is.
*/
struct SocioEdit {
    int model;
    int values[MaxSocialEffectNum];
};

/*
Gross +4 on all three, the same as Police State, Democratic, Green and Power.
The two with a project escape pay -4 until they reach it; Eudaimonic, which
has none, pays -2 forever -- exactly what every other row in the table pays.

Written out in full rather than as a diff, so what the row becomes is readable
here without holding the stock values in your head.
*/
static const SocioEdit SocioRepriced[] = {
    // ECON EFFIC SUPP TAL MOR POL GROW PLAN PROBE IND RES
    { SOCIAL_M_CYBERNETIC,
      {    0,   2,   0,  0,  0, -2,  -2,   0,    0,  0,  2 } },
    { SOCIAL_M_EUDAIMONIC,
      {    2,   0,   0,  0, -2,  0,   2,   0,    0,  0,  0 } },
    { SOCIAL_M_THOUGHT_CONTROL,
      {    0,   0,  -2,  0,  2,  2,   0,   0,    0,  0, -2 } },
};

/*
The table as alphax.txt actually loaded it, whatever that was.

Restoring from a hardcoded stock row would be a lie on any install that ran
./install.sh --se-rebalance, where the file on disk already holds the new
values -- "restore" has to mean "what this game started with", not "what the
unmodded game ships".
*/
static CSocialEffect socio_loaded[MaxSocialModelNum];
static bool socio_have_loaded = false;

static void socio_snapshot() {
    if (socio_have_loaded) {
        return;
    }
    for (int m = 0; m < MaxSocialModelNum; m++) {
        socio_loaded[m] = SocialField[SOCIAL_C_FUTURE].soc_effect[m];
    }
    socio_have_loaded = true;
}

static bool socio_is_repriced() {
    for (auto& e : SocioRepriced) {
        const CSocialEffect& live = SocialField[SOCIAL_C_FUTURE].soc_effect[e.model];
        for (int i = 0; i < MaxSocialEffectNum; i++) {
            if (live.values[i] != e.values[i]) {
                return false;
            }
        }
    }
    return true;
}

/*
Recompute every faction's effect vector from the choices it already holds.

Without this the new table sits there doing nothing until something else
happens to recalculate -- the player would throw the switch, see the social
screen change, and find their actual economy unchanged until next turn.

NOT social_upkeep(), which is the turn-boundary routine and starts by copying
pending over current. Calling it here would COMMIT a social change the player
had chosen and not yet paid the upheaval for. Only the derived vectors are
touched; every choice is left exactly as it was.
*/
static void socio_recalc() {
    for (int i = 1; i < MaxPlayerNum; i++) {
        if (!is_alive(i)) {
            continue;
        }
        Faction* f = &Factions[i];
        auto current = (CSocialCategory*)&f->SE_Politics;
        auto pending = (CSocialCategory*)&f->SE_Politics_pending;
        social_calc(current, (CSocialEffect*)&f->SE_economy, i, false, false);
        social_calc(pending, (CSocialEffect*)&f->SE_economy_pending, i, false, false);
        social_calc(pending, (CSocialEffect*)&f->SE_economy_2, i, true, false);
    }
}

static void socio_set(bool repriced) {
    socio_snapshot();
    for (int m = 0; m < MaxSocialModelNum; m++) {
        SocialField[SOCIAL_C_FUTURE].soc_effect[m] = socio_loaded[m];
    }
    if (repriced) {
        for (auto& e : SocioRepriced) {
            CSocialEffect& live = SocialField[SOCIAL_C_FUTURE].soc_effect[e.model];
            for (int i = 0; i < MaxSocialEffectNum; i++) {
                live.values[i] = e.values[i];
            }
        }
    }
    socio_recalc();
    ch_log("[socio] Future Society table: %s\n", repriced ? "repriced" : "as loaded");
}

// "++EFFIC, --GROWTH, --POLICE" -- the row as the game currently holds it.
static void socio_row(int model, char* out, size_t out_len) {
    static const char* Names[MaxSocialEffectNum] = {
        "ECONOMY", "EFFIC", "SUPPORT", "TALENT", "MORALE", "POLICE",
        "GROWTH", "PLANET", "PROBE", "INDUSTRY", "RESEARCH"
    };
    const CSocialEffect& e = SocialField[SOCIAL_C_FUTURE].soc_effect[model];
    out[0] = '\0';
    for (int i = 0; i < MaxSocialEffectNum; i++) {
        int v = e.values[i];
        if (!v) {
            continue;
        }
        char pips[8];
        int n = v < 0 ? -v : v;
        if (n > 5) {
            n = 5;
        }
        for (int k = 0; k < n; k++) {
            pips[k] = v < 0 ? '-' : '+';
        }
        pips[n] = '\0';
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, "%s%s%s",
            used ? ", " : "", pips, Names[i]);
    }
    if (!out[0]) {
        strcpy_n(out, out_len, "no effects");
    }
}

static void chiron_show_socio() {
    socio_snapshot();

    syn_parse_state_t saved;
    parse_state_save(&saved);

    for (;;) {
        bool on = socio_is_repriced();

        char cyber[128], eudaim[128], thought[128];
        socio_row(SOCIAL_M_CYBERNETIC, cyber, sizeof(cyber));
        socio_row(SOCIAL_M_EUDAIMONIC, eudaim, sizeof(eudaim));
        socio_row(SOCIAL_M_THOUGHT_CONTROL, thought, sizeof(thought));

        char l_cyber[160], l_eudaim[160], l_thought[160];
        snprintf(l_cyber, sizeof(l_cyber), "Cybernetic: %s", cyber);
        snprintf(l_eudaim, sizeof(l_eudaim), "Eudaimonic: %s", eudaim);
        snprintf(l_thought, sizeof(l_thought), "Thought Control: %s", thought);

        parse_says(1, l_cyber, -1, -1);
        parse_says(2, l_eudaim, -1, -1);
        parse_says(3, l_thought, -1, -1);
        parse_says(4, on
            ? "Repriced. Every row costs what the rest of the table costs."
            : "The table as this game loaded it.", -1, -1);
        /*
        Say the two things that are genuinely surprising, in the place where
        the decision is actually made. Both were buried in a text file nobody
        reads before flipping a switch.
        */
        parse_says(5, "Network Backbone voids Cybernetic's penalties; Cloning "
            "Vats voids Thought Control's. Eudaimonic always pays.", -1, -1);
        parse_says(6, "This session only. install.sh --se-rebalance makes it "
            "permanent.", -1, -1);
        parse_says(7, on ? "Restore the table as loaded."
                         : "Apply the repricing.", -1, -1);

        int choice = X_pop_2("modmenu", "CHIRONSOCIO", 0);
        if (choice == 1) {
            socio_set(!on);
        } else {
            break;
        }
    }

    parse_state_restore(&saved);
}

// ── the mod's own menu ─────────────────────────────────────────────────────

/*
Alt+M. Says whether the mod is actually working, and lets the switches be
thrown without a restart.

The reason this earns its place is the first line of it. Every failure path in
this file falls back to the game's own text -- deliberately, because a dialogue
box coming up empty would be worse than a canned line. The consequence is that
a broken install and a working one look the same from the seat: vanilla
dialogue is both "the bridge is down" and "the mod is not installed", and there
is nothing on screen that separates them. The player's diagnosis is a log file
they have no reason to know exists. A status line turns the mod's best property
into something visible.

The toggles are the session's, not the file's. Flipping one here does not write
chiron.ini, because a menu that silently edits a config the player also hand-
edits is a good way to lose their comments; chiron.ini stays the thing that
decides how the game starts, and this decides how it is behaving right now.
*/
static void menu_status(char* line1, size_t len1, char* line2, size_t len2) {
    snprintf(line1, len1, "Backend: %s at %s:%d.",
        chiron_conf.backend, chiron_conf.host, chiron_conf.port);

    if (!chiron_conf.enabled) {
        strcpy_n(line2, len2,
            "Generated text is off. Every line is the game's own.");
    } else if (!backend.ever) {
        strcpy_n(line2, len2,
            "Not called yet. Nothing has needed generating this session.");
    } else if (backend.last_ok) {
        snprintf(line2, len2, "Working. Last reply %d.%ds, %d call%s, %d failed.",
            (int)(backend.last_ms / 1000), (int)((backend.last_ms % 1000) / 100),
            backend.calls, backend.calls == 1 ? "" : "s", backend.failures);
    } else {
        snprintf(line2, len2, "DOWN -- %s. Showing the game's own text.",
            backend.reason);
    }
}

/*
Ask the backend for one word and report what came back.

The reply is shown rather than just its success, because the failure this
catches most often is not a dead server but the WRONG one: point `backend=` at
a llama.cpp when ollama is what is listening and the request succeeds, the
parse finds no key it recognises, and every dialogue silently goes vanilla. A
visible "ready" proves the whole path end to end -- socket, request shape,
reply key -- in a way that a green light cannot.
*/
static void menu_test_backend() {
    char reply[512];
    bool ok = http_generate("Reply with one word: ready\n", reply, sizeof(reply), 8);

    char msg[256];
    if (!ok) {
        snprintf(msg, sizeof(msg), "No answer: %s", backend.reason);
    } else {
        strip_preamble(reply);
        /*
        Flatten before it is handed to parse_says. A '$' would be read as a
        substitution on its way to the screen and render as some other slot's
        value -- the same reason generated dialogue is scrubbed of tokens.
        */
        char flat[96];
        size_t j = 0;
        for (const char* p = reply; *p && j + 1 < sizeof(flat); p++) {
            if (*p == '$') {
                continue;
            }
            flat[j++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
        }
        flat[j] = '\0';
        snprintf(msg, sizeof(msg), "Answered in %d.%ds: %s",
            (int)(backend.last_ms / 1000), (int)((backend.last_ms % 1000) / 100),
            flat);
    }
    ch_log("[menu] backend test: %s\n", msg);
    chiron_notice("Chiron Rising", msg);
}

void chiron_show_menu() {
    chiron_ensure_init();

    /*
    The dispatch is launched AFTER the menu closes, not from inside the loop.
    It shows a popup of its own, and opening one of ours while ours is still up
    is the one arrangement here that has no precedent elsewhere in the file --
    the probe confrontation is careful to restore the parse state before it
    shows its second box for the same reason.
    */
    bool dispatch = false;

    syn_parse_state_t saved;
    parse_state_save(&saved);

    for (;;) {
        char line1[128], line2[192];
        menu_status(line1, sizeof(line1), line2, sizeof(line2));

        char t_dialogue[64], t_names[64], t_probe[64];
        snprintf(t_dialogue, sizeof(t_dialogue), "Generated dialogue: %s.",
            chiron_conf.enabled ? "on" : "off");
        snprintf(t_names, sizeof(t_names), "Base names from faction culture: %s.",
            chiron_conf.base_names ? "on" : "off");
        snprintf(t_probe, sizeof(t_probe), "Object to probe teams: %s.",
            chiron_conf.probe_protests ? "on" : "off");

        parse_says(1, line1, -1, -1);
        parse_says(2, line2, -1, -1);
        parse_says(3, "Planetnet dispatch.", -1, -1);
        parse_says(4, "Test the connection now.", -1, -1);
        char t_socio[64];
        snprintf(t_socio, sizeof(t_socio), "Future Society repricing: %s...",
            socio_is_repriced() ? "on" : "off");

        parse_says(5, t_dialogue, -1, -1);
        parse_says(6, t_names, -1, -1);
        parse_says(7, t_probe, -1, -1);
        parse_says(8, t_socio, -1, -1);

        // Named file, for the reason given at the probe confrontation: bare
        // X_pop resolves against Script.txt, and this label is in modmenu.txt.
        int choice = X_pop_2("modmenu", "CHIRONMENU", 0);
        if (choice == 1) {
            dispatch = true;
            break;
        } else if (choice == 2) {
            menu_test_backend();
        } else if (choice == 3) {
            chiron_conf.enabled = !chiron_conf.enabled;
            ch_log("[menu] generated dialogue %s\n",
                chiron_conf.enabled ? "on" : "off");
        } else if (choice == 4) {
            chiron_conf.base_names = !chiron_conf.base_names;
            ch_log("[menu] culture base names %s\n",
                chiron_conf.base_names ? "on" : "off");
        } else if (choice == 5) {
            chiron_conf.probe_protests = !chiron_conf.probe_protests;
            ch_log("[menu] probe protests %s\n",
                chiron_conf.probe_protests ? "on" : "off");
        } else if (choice == 6) {
            /*
            Opened from inside the loop, unlike the dispatch. It is a popup of
            ours over a popup of ours, which the news dispatch deliberately
            avoids -- but this one has to come back HERE, so that closing the
            submenu returns to the menu you opened it from rather than to the
            map. It saves and restores its own parse slots, so ours survive.
            */
            chiron_show_socio();
        } else {
            break;  // Close, or the box dismissed some other way.
        }
    }

    parse_state_restore(&saved);
    if (dispatch) {
        chiron_show_news();
    }
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
    // 0 means "not set in the ini", resolved against the backend below.
    chiron_conf.port = 0;
    strcpy_n(chiron_conf.backend, sizeof(chiron_conf.backend), "bridge");
    strcpy_n(chiron_conf.model, sizeof(chiron_conf.model), "llama3.2");
    strcpy_n(chiron_conf.host, sizeof(chiron_conf.host), "127.0.0.1");
    chiron_conf.timeout_ms = 8000;
    /*
    A dialogue box holds at most 6 short lines, which is nowhere near 320
    tokens, and the game blocks on every one of these. Generation time scales
    with what the model is allowed to emit, so the budget is the cheapest lever
    on the pause. Raise it in chiron.ini if replies start getting clipped.
    */
    chiron_conf.max_tokens = 110;
    chiron_conf.cache_size = 64;
    chiron_conf.base_names = 1;
    chiron_conf.probe_protests = 1;
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
            else if (!_stricmp(key, "backend"))     strcpy_n(chiron_conf.backend, sizeof(chiron_conf.backend), val);
            else if (!_stricmp(key, "model"))       strcpy_n(chiron_conf.model, sizeof(chiron_conf.model), val);
            else if (!_stricmp(key, "host"))        strcpy_n(chiron_conf.host, sizeof(chiron_conf.host), val);
            else if (!_stricmp(key, "timeout_ms"))  chiron_conf.timeout_ms = atoi(val);
            else if (!_stricmp(key, "max_tokens"))  chiron_conf.max_tokens = atoi(val);
            else if (!_stricmp(key, "base_names"))  chiron_conf.base_names = atoi(val);
            else if (!_stricmp(key, "probe_protests")) chiron_conf.probe_protests = atoi(val);
            else if (!_stricmp(key, "debug"))       chiron_conf.debug = atoi(val);
        }
        fclose(f);
    }

    /*
    Each backend listens somewhere different, and making the user remember that
    is a way to have the mod look broken. Only fall back to the default when the
    ini did not say.
    */
    if (!chiron_conf.port) {
        chiron_conf.port = !_stricmp(chiron_conf.backend, "ollama")   ? 11434
                         : !_stricmp(chiron_conf.backend, "llamacpp") ? 8080
                         : 11436;
    }

    if (chiron_conf.debug) {
        chiron_log = fopen("chiron.txt", "w");
    }

    chiron_trace("init: enabled=%d %s:%d timeout=%dms debug=%d\n",
        chiron_conf.enabled, chiron_conf.host, chiron_conf.port,
        chiron_conf.timeout_ms, chiron_conf.debug);

    /*
    Bring winsock up here, at the first text lookup during startup, rather than
    at the first generation.

    Deferring it that far was a workaround for a startup crash that turned out
    to have nothing to do with winsock -- it was a stale modmenu.txt. What
    deferring actually bought was a LoadLibraryA("ws2_32.dll") executed deep
    inside a modal diplomacy dialog, which hung the game outright: the trace
    stopped at "hook: rewriting ..." and never reached the line below.

    Here we are on the game's normal startup path with no dialog open, which is
    a safe place to pull in a DLL. If it fails, winsock_ready stays false and
    every generation falls back to vanilla text.
    */
    if (chiron_conf.enabled) {
        ensure_winsock();
    }
    ch_log("chiron_init: enabled=%d backend=%s %s:%d timeout=%dms winsock=%d\n",
        chiron_conf.enabled, chiron_conf.backend, chiron_conf.host,
        chiron_conf.port, chiron_conf.timeout_ms, (int)winsock_ready);

}

