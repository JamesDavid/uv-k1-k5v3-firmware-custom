/* OnePress decoder: one keypad press per letter, a word-pair model picks the letters.
 *
 * Portable C99 with no allocation: the model is read in place (flash, a memory map or a
 * buffer) and all working memory lives in an op_decoder the caller provides. Mirrors
 * web/decoder.js exactly; tests/parity.js checks the two agree.
 *
 * Usage:
 *   static op_model model;   static op_decoder dec;
 *   op_model_open(&model, blob, blob_size);           // blob: 4-byte aligned KPD2 file
 *   op_decode(&dec, &model, "66069092904663", 8, NULL, NULL, 0);
 *   op_render(&dec, &model, NULL, out, sizeof out);   // "On my way home"
 *
 * Key strings use the same characters as decoder.js: '2'-'9' predictive letters, '0'
 * space, '#' punctuation (an apostrophe between letters), and from exact mode the literal
 * characters 'a'-'z', '\'', '.', ',', '?', '!' and 'A'-'J' for the digits 0-9 (exact mode or
 * a held number key). Digits form numbers: #6#000#00 renders "$6,000.00".
 *
 * Memory: sizeof(op_decoder) is about 11 KB with the defaults below. Small targets can
 * lower the limits, e.g. -DOP_MAX_BEAM=4 -DOP_MAX_SLOTS=48 -DOP_MAX_KEYS=160 for ~3.4 KB, or
 * -DOP_MAX_BEAM=3 -DOP_MAX_SLOTS=24 -DOP_MAX_KEYS=72 (a 67-character radio message) for ~1.6 KB.
 * -DOP_NO_QWERTY leaves out matching the letters tapped on a QWERTY layout (start.hint is then
 * only used for words no dictionary word matches), for keypad-only devices.
 * The model must be little-endian (it is on ARM, x86, ESP32 and RISC-V).
 *
 * -DOP_EXT_MODEL: the model sits in storage the CPU can't address directly (a radio's SPI
 * flash). Give op_model_open its storage address as data ((const void *)(uintptr_t)addr) and
 * supply op_ext_read below; the model's arrays then hold storage addresses. Word texts are
 * read OP_WORD_BUF bytes at most. op_word copies into a buffer of its own, valid until the
 * next call. Decoding gives the same results either way.
 */
#ifndef ONEPRESS_H
#define ONEPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef OP_EXT_MODEL
#define OP_REF(T) uint32_t      /* a storage address */
#define OP_WORD_BUF 64          /* longest word text read */
/* Copy n bytes at storage address addr to dst. Supplied by the platform. */
void op_ext_read(uint32_t addr, void *dst, uint32_t n);
#else
#define OP_REF(T) const T *
#define OP_WORD_BUF 1           /* texts are read in place */
#endif

#ifndef OP_MAX_KEYS
#define OP_MAX_KEYS 256         /* key presses in one composition */
#endif
#ifndef OP_MAX_SLOTS
#define OP_MAX_SLOTS 96         /* words + punctuation marks in one composition */
#endif
#ifndef OP_MAX_BEAM
#define OP_MAX_BEAM 16          /* largest beam op_decode accepts; 4-8 is plenty */
#endif
#define OP_MAX_EXACT 40         /* dictionary words considered per slot */
#define OP_MAX_COMPLETIONS 12   /* extra prefix completions for the word being typed */
#ifndef OP_USER_WORDS
#define OP_USER_WORDS 256       /* words a person can teach it (op_user) */
#endif
#ifndef OP_USER_PAIRS
#define OP_USER_PAIRS 1024      /* token and token-pair counts kept for a person */
#endif
#define OP_USER_WORD_LEN 23     /* longest learned word */
#define OP_MAX_USER_CANDS 4     /* learned words considered per slot, and again as completions */
#define OP_RECENT_WORDS 32      /* words of start->before that count as recent */
#define OP_MAX_SLIPS 8          /* words one keypad slip away considered per slot */
#define OP_MAX_CANDS (OP_MAX_EXACT + OP_MAX_COMPLETIONS + 2 * OP_MAX_USER_CANDS + OP_MAX_SLIPS)

#define OP_NONE 0xFFFF          /* token id of an unknown word (decoder.js uses -1) */
#define OP_TIME_ID 0xFFFA       /* id of a number read as a time (5:30); lock a slot to it, or to
                                   OP_NONE for the plain number (5.30) */
#define OP_COMPOUND_ID 0xFFF0   /* ids of known words run together (lollypops), OP_COMPOUND_ID + 0..5 in
                                   order of preference */
#define OP_TIME_BEFORE 10       /* words after which a number is likely a time (at, by, until...) */

enum { OP_OK = 0, OP_ERR_MODEL = -1, OP_ERR_TOO_LONG = -2, OP_ERR_SPACE = -3, OP_ERR_ARG = -4 };

/* What each slot was decoded as. */
enum { OP_WORD, OP_PARTIAL, OP_PUNCT, OP_LITERAL, OP_UNKNOWN, OP_NUMBER, OP_TIME, OP_COMPOUND };

enum { OP_P_QQ, OP_P_QN, OP_P_DQ, OP_P_CF, OP_P_DB, OP_P_CB, OP_P_EB,
       OP_P_APOSTROPHE, OP_P_COMPLETION, OP_P_LITERAL, OP_P_UNKNOWN,
       OP_P_SLIP_SUB, OP_P_SLIP_DUP, OP_P_FLAGS,   /* optional in the file (older ones stop before them) */
       OP_N_PARAMS };

typedef struct {
    uint16_t scale, n_words, n_punct, bos;
    OP_REF(char) word_text;
    OP_REF(uint32_t) word_off;
    OP_REF(uint16_t) key_idx;
    OP_REF(uint16_t) strip_idx;
    uint32_t n_strip;
    OP_REF(uint16_t) disp_id;
    OP_REF(uint32_t) disp_off;
    OP_REF(char) disp_text;
    uint32_t n_disp;
    OP_REF(int16_t) uni;
    OP_REF(int16_t) bow;
    OP_REF(int16_t) lp;
    OP_REF(uint32_t) row_start;
    OP_REF(uint16_t) next;
    OP_REF(uint16_t) q_start;
    uint32_t n_q;
    OP_REF(char) punct;
    int16_t param[OP_N_PARAMS];
    uint16_t time_id[2];        /* "am", "pm" (OP_NONE if absent) */
    uint16_t time_before[OP_TIME_BEFORE];   /* "at", "by", ... (OP_NONE if absent) */
    /* Optional sections (empty in older model files). */
    OP_REF(uint16_t) q_skip;     /* greetings skipped when finding a sentence's opener */
    uint32_t n_skip;
    OP_REF(uint32_t) tri_key;    /* three-word contexts p2 << 16 | p1 */
    OP_REF(uint32_t) tri_start;
    OP_REF(int16_t) tri_bow;
    OP_REF(int16_t) tri_lp;
    OP_REF(uint16_t) tri_next;
    uint32_t n_tri;
    OP_REF(uint32_t) cap_key;    /* prev << 16 | word: the word's other casing after prev */
    uint32_t n_cap;
    OP_REF(uint16_t) alt_id;
    OP_REF(uint32_t) alt_off;
    OP_REF(char) alt_text;
    uint32_t n_alt;
    OP_REF(uint32_t) cap_next;   /* word << 16 | next: the word's other casing before next */
    uint32_t n_cap_next;
    OP_REF(uint32_t) cap_keep;   /* word << 16 | next: its usual casing before next, whatever prev says */
    uint32_t n_cap_keep;
    OP_REF(int8_t) cmp_head;   /* words run together: start / end lift in 1/4 nat, or NULL */
    OP_REF(int8_t) cmp_tail;
    uint8_t elided;             /* has words ending in an apostrophe (French l', qu'): see op_decode */
    OP_REF(uint32_t) cmp_known;          /* first << 16 | second of words known written as one (older files) */
    uint32_t n_known;
    OP_REF(uint32_t) cmp_bloom;          /* the same as a Bloom filter: nBits, k, bits; or NULL */
} op_model;

/* What one person types, learned by op_learn and read by op_decode: words the dictionary
 * lacks, call signs with digits included (spelled once in exact mode, predicted after that;
 * learned word k has id bos + 1 + k)
 * and counts of each token and token pair they used. Save it with op_user_save and restore it
 * with op_user_load (a versioned format that survives changes to these limits or the compiler);
 * it belongs to the model it was made for (bos).
 * About 12 KB with the defaults; -DOP_USER_WORDS=64 -DOP_USER_PAIRS=256 gives ~3 KB. */
typedef struct {
    uint16_t prev, cur;         /* cur's own count uses prev 0xFFFD */
    uint8_t count;
} op_user_pair;

typedef struct {
    uint16_t bos, n_words, n_pairs;
    uint32_t stamp;             /* changes whenever it learns (op_decode's resume point checks it) */
    char word[OP_USER_WORDS][OP_USER_WORD_LEN + 1];     /* NUL-terminated */
    op_user_pair pair[OP_USER_PAIRS];                   /* sorted by (prev, cur) */
} op_user;

/* Optional neural word model (KPN1 file, model/export_nn.py): a small GRU that scores each
 * candidate word given everything before it. Read in place like op_model. For phones; it
 * needs ~55 KB of working memory (op_nn_work, one per op_decoder) and about 0.3M multiply-adds per hypothesis
 * per word. Integer only, so results match decoder.js exactly. -DOP_NO_NN leaves it out
 * (op_nn_open then returns OP_ERR_ARG). */
#ifndef OP_NN_MAX
#define OP_NN_MAX 256           /* largest embedding / hidden size */
#endif
#define OP_NN_LINE 32           /* tokens of start->before's current line it reads first */
typedef struct {
    uint16_t E, H, V, bos, unk, model_bos;
    int16_t weight;             /* how much its score counts for words, in 1/256 */
    int16_t punct_weight;       /* ... and for punctuation (0 in older files: the rules alone) */
    const int16_t *sig, *bias;
    const int8_t *emb, *wih, *whh, *proj;
    const uint8_t *es, *is, *hs, *ps;   /* per-row shifts of emb, wih, whh, proj */
    const int32_t *bih, *bhh, *bproj;
} op_nn;

typedef struct {
    int16_t h[2][OP_MAX_BEAM][OP_NN_MAX];   /* the beam's states, double-buffered */
    int16_t p[2][OP_MAX_BEAM][OP_NN_MAX];   /* and their projections */
    int32_t g[6 * OP_NN_MAX];
    int16_t rh[OP_MAX_BEAM][OP_NN_MAX], rp[OP_MAX_BEAM][OP_NN_MAX];   /* at op_decode's resume point */
    const void *rs_owner;                   /* the op_decoder that saved them */
} op_nn_work;

/* Context before the composition, from the text already in the field. */
typedef struct {
    uint16_t prev;      /* previous token id: model->bos at a sentence start, OP_NONE if unknown */
    uint8_t cap;        /* capitalize the first word */
    const op_user *user;    /* what this person has typed, or NULL */
    /* The text before the composition, or NULL: what's already in the field, or the last few
     * messages of the conversation, one per line. Its last words are the context (they replace
     * prev and cap), and words used in it get a small bonus. A line break ends a sentence. */
    const char *before;
    /* On a letter keyboard that sends keypad digits: the letter tapped for each key press (UTF-8,
     * one character per press; any other character for presses that weren't letters), or NULL.
     * Matched letter by letter (neighboring keys forgiven), and shown, and learned when sent,
     * when no word matches. */
    const char *hint;
    /* The neural word model and its working memory, or NULL (op_nn_open). */
    const op_nn *nn;
    op_nn_work *nn_work;
    /* That keyboard's letter rows, top to bottom, separated by '|' (UTF-8), or NULL for English
     * QWERTY: "qwertzuiopü|asdfghjklöä|yxcvbnm", "йцукенгшщзхъ|фывапролджэ|ячсмитьбю". Row r is
     * shifted right by r/2 key; keys one apart in a row, or touching in the next row, neighbor. */
    const char *layout;
} op_start;

/* Force slot `slot` to word `id` (as decoder.js locks do). Ignored if that word isn't a candidate. */
typedef struct {
    uint16_t slot, id;
} op_lock;

typedef struct {
    uint8_t type;       /* internal token type */
    char lit;           /* punctuation typed exactly, or 0; a word joined to the one before by a hyphen: '-' */
    uint16_t off, len;  /* word: span of keys[] / lits[]; space: its position in the keys given */
} op_tok;

typedef struct {
    uint16_t id;        /* token id; OP_NONE for OP_LITERAL / OP_UNKNOWN / OP_NUMBER, OP_TIME_ID for OP_TIME,
                           OP_COMPOUND_ID + rank for OP_COMPOUND */
    uint8_t kind;       /* OP_WORD, OP_PARTIAL, ... */
} op_choice;

typedef struct {
    int32_t score;
    uint16_t seq, prev, id, tri;
    uint8_t parent, kind, first, cf;
} op_entry;

typedef struct {
    /* Results of the last op_decode. */
    uint16_t n_toks, n_slots;
    op_tok toks[OP_MAX_KEYS];
    uint16_t slot_tok[OP_MAX_SLOTS];
    op_choice choice[OP_MAX_SLOTS];
    int32_t score;
    const op_user *user;    /* start->user, if it matches the model */
    uint16_t ctx_prev;      /* the context decoding started from (start->before, or prev / cap) */
    uint8_t ctx_cap, n_recent;
    uint16_t recent[OP_RECENT_WORDS];   /* words used in start->before */
    uint16_t line[OP_NN_LINE];          /* start->before's current line, for the network */
    uint8_t n_line;
    /* Working memory. */
    char keys[OP_MAX_KEYS];
    uint16_t lits[OP_MAX_KEYS];         /* exact letters (code points, lowercase), 0 for predictive */
    uint16_t hints[OP_MAX_KEYS];        /* letters tapped (code points, lowercase), 0 for none */
    const char *layout;                 /* start->layout, or the English rows */
    struct { uint16_t id; uint8_t parent, kind; } bp[OP_MAX_SLOTS][OP_MAX_BEAM];
    op_entry ent[OP_MAX_BEAM];          /* the best extensions of the beam (op_decode) */
    /* The search after the last finished sentence of the previous op_decode, to resume from
     * when the next call's keys only add to it (see op_decode). */
    struct {
        uint8_t ok, nb;
        uint16_t n_keys, slot;
        uint64_t fp;            /* model, start, locks up to the slot, beam */
        int32_t score[OP_MAX_BEAM];
        uint16_t prev[OP_MAX_BEAM], tri[OP_MAX_BEAM];
        uint8_t first[OP_MAX_BEAM], cf[OP_MAX_BEAM];
        char keys[OP_MAX_KEYS], hint[OP_MAX_KEYS];
    } rs;
} op_decoder;

/* Point `m` at a KPD2 model. `data` must stay valid and be 4-byte aligned. */
int op_model_open(op_model *m, const void *data, size_t size);

/* Decode a key string (NUL-terminated). beam: 1..OP_MAX_BEAM. start/locks may be NULL.
 * Typing one key at a time, call it again with the longer string: when everything up to the
 * last finished sentence is unchanged (the keys, the QWERTY letters, start, its user and
 * network, locks up to there, beam), it resumes the search after that sentence instead of
 * starting over, with exactly the same result. A finished sentence is a . ! ? (or #) followed
 * by a space and more. */
int op_decode(op_decoder *d, const op_model *m, const char *keys, int beam,
              const op_start *start, const op_lock *locks, int n_locks);

/* The candidate ids of slot `slot` in the last op_decode (most frequent first, at most max):
 * lock one to show another guess. Returns how many. */
int op_candidates(const op_decoder *d, const op_model *m, int slot, uint16_t *ids, int max);

/* Write the decoded text (NUL-terminated) and return its length, or OP_ERR_SPACE. */
int op_render(const op_decoder *d, const op_model *m, const op_start *start, char *out, int cap);

/* Word text of token `id` (not NUL-terminated); returns its length. */
int op_word(const op_model *m, uint16_t id, const char **text);

/* Point `nn` at a KPN1 network made for model m (4-byte aligned, kept valid while used). */
int op_nn_open(op_nn *nn, const void *data, size_t size, const op_model *m);

/* Start an empty op_user for model m. */
void op_user_init(op_user *u, const op_model *m);

/* Learn from the last op_decode once the person accepts the text (sends it). Pass the same
 * start as op_decode. Completions still being typed and unmatched keys are skipped; a word
 * spelled exactly that the dictionary lacks becomes a learned word. */
int op_learn(op_user *u, const op_decoder *d, const op_model *m, const op_start *start);

/* Save u as bytes ("KPU1" format, see onepress.c): returns the length, or OP_ERR_SPACE if cap is
 * too small (op_user_save(u, NULL, 0) returns the length needed). */
int op_user_save(const op_user *u, uint8_t *out, int cap);

/* Restore bytes from op_user_save into u. OP_ERR_MODEL if they aren't a saved user for model m
 * (u is then empty). Saved with larger limits, the extra words and pairs are dropped. */
int op_user_load(op_user *u, const op_model *m, const uint8_t *data, size_t size);

#endif
