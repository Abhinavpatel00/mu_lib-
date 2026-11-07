/*
 * poker_bench.c
 * =============================================================
 * Benchmark common poker card operations on the 8-byte Card struct:
 *
 *   Card layout (8 bytes = 1 machine word):
 *   ┌────────────────┬──────────┬────────────────┐
 *   │  next (32-bit) │ info (8b)│  title[3]      │
 *   └────────────────┴──────────┴────────────────┘
 *
 *   info byte:  [7:6]=tag  [5:2]=rank(0-12)  [1:0]=suit(0-3)
 *
 * Operations benchmarked:
 *   1. Rank extraction    – bitshift vs LUT
 *   2. Suit extraction    – bitshift vs mask
 *   3. Flush detection    – field loop vs word-mask trick
 *   4. Straight detection – sort+check vs bitmask
 *   5. Hand sorting       – insertion sort vs network sort (branchless)
 *   6. Full hand eval     – classify 5-card poker hand
 *   7. Deck shuffle       – Fisher-Yates
 *   8. Card equality      – 64-bit word vs field-by-field (baseline)
 *
 * Compile:
 *   gcc -O2 -march=native -std=c11 -Wall poker_bench.c -o poker_bench
 * =============================================================
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════
 *  Card struct — 8 bytes, no padding
 * ═══════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint32_t next;          /* linked-list / deck index     */
    uint8_t  info;          /* tag:2 | rank:4 | suit:2      */
    char     title[3];      /* "AH\0", "2C\0", "KS\0" etc  */
} Card;
_Static_assert(sizeof(Card) == 8, "Card must be 8 bytes");

/* ── Info byte packing ─────────────────────────────────── */
#define SUIT_MASK   0x03u
#define RANK_SHIFT  2
#define RANK_MASK   0x3Cu   /* bits [5:2] */

/* suits */
#define CLUBS    0
#define DIAMONDS 1
#define HEARTS   2
#define SPADES   3

/* ranks 0–12 → 2,3,4,...,K,A */
#define RANK_2  0
#define RANK_ACE 12

static inline uint8_t make_info(uint8_t rank, uint8_t suit) {
    return (uint8_t)((rank << RANK_SHIFT) | suit);
}
static inline uint8_t info_suit(uint8_t info)  { return info & SUIT_MASK; }
static inline uint8_t info_rank(uint8_t info)  { return (info >> RANK_SHIFT) & 0xF; }

/* ── Timing ─────────────────────────────────────────────── */
static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Card word view ─────────────────────────────────────── */
static inline uint64_t card_word(const Card *c) {
    uint64_t v; memcpy(&v, c, 8); return v;
}

/* ═══════════════════════════════════════════════════════
 *  1. Rank extraction
 * ═══════════════════════════════════════════════════════ */
/* LUT: index = info byte (256 entries, only 52 used) */
static uint8_t rank_lut[256];
static void build_rank_lut(void) {
    for (int i = 0; i < 256; ++i) rank_lut[i] = (i >> RANK_SHIFT) & 0xF;
}

__attribute__((noinline))
uint32_t bench_rank_bitshift(const Card *deck, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) sum += info_rank(deck[i].info);
    return sum;
}

__attribute__((noinline))
uint32_t bench_rank_lut(const Card *deck, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) sum += rank_lut[(uint8_t)deck[i].info];
    return sum;
}

/* ═══════════════════════════════════════════════════════
 *  2. Suit extraction
 * ═══════════════════════════════════════════════════════ */
__attribute__((noinline))
uint32_t bench_suit_mask(const Card *deck, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) sum += info_suit(deck[i].info);
    return sum;
}

/* ═══════════════════════════════════════════════════════
 *  3. Flush detection (all 5 cards same suit)
 * ═══════════════════════════════════════════════════════ */
typedef Card Hand[5];

/* Method A: check each card's suit field */
__attribute__((noinline))
bool flush_field(const Hand h) {
    uint8_t s = info_suit(h[0].info);
    return info_suit(h[1].info) == s
        && info_suit(h[2].info) == s
        && info_suit(h[3].info) == s
        && info_suit(h[4].info) == s;
}

/*
 * Method B: word-mask trick.
 * Since suit is bits[1:0] of the info byte (byte 4 of the card word),
 * we can extract all 5 suits via bitwise ops on the 5 uint64_t words.
 *
 * Byte 4 of each word holds info.  Mask = 0x0000000300000000ULL shifted.
 * Then OR-fold: if all suits equal, (s0^s1)|(s0^s2)|... == 0.
 */
#define INFO_BYTE_OFFSET 4
#define SUIT_IN_WORD  ((uint64_t)SUIT_MASK << (INFO_BYTE_OFFSET * 8))

__attribute__((noinline))
bool flush_word(const Hand h) {
    uint64_t w0,w1,w2,w3,w4;
    memcpy(&w0,&h[0],8); memcpy(&w1,&h[1],8);
    memcpy(&w2,&h[2],8); memcpy(&w3,&h[3],8);
    memcpy(&w4,&h[4],8);
    uint64_t suits = (w0 | w1 | w2 | w3 | w4) & SUIT_IN_WORD;
    uint64_t must  = (w0 & w1 & w2 & w3 & w4) & SUIT_IN_WORD;
    return suits == must;   /* all bits identical ↔ all same suit */
}

/* ═══════════════════════════════════════════════════════
 *  4. Straight detection (5 consecutive ranks)
 * ═══════════════════════════════════════════════════════ */

/* Method A: sort then check gaps (with branches) */
static void sort5_ranks(uint8_t r[5]) {
    /* simple insertion sort on 5 elements */
    for (int i = 1; i < 5; ++i) {
        uint8_t key = r[i]; int j = i-1;
        while (j >= 0 && r[j] > key) { r[j+1] = r[j]; --j; }
        r[j+1] = key;
    }
}

__attribute__((noinline))
bool straight_sort(const Hand h) {
    uint8_t r[5];
    for (int i = 0; i < 5; ++i) r[i] = info_rank(h[i].info);
    sort5_ranks(r);
    /* wheel straight: A-2-3-4-5 */
    if (r[4] == RANK_ACE && r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3)
        return true;
    return (r[1]-r[0]==1) & (r[2]-r[1]==1) & (r[3]-r[2]==1) & (r[4]-r[3]==1);
}

/* Method B: rank bitmask — set bit per rank, check for 5 consecutive */
__attribute__((noinline))
bool straight_bitmask(const Hand h) {
    uint16_t bits = 0;
    for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << info_rank(h[i].info));
    /* wheel: ace can also be low */
    if (bits & (1u << RANK_ACE)) bits |= 1u; /* duplicate ace as rank -1 */
    /* check 5 consecutive set bits */
    for (int shift = 0; shift <= 9; ++shift) {
        if ((bits >> shift & 0x1F) == 0x1F) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════
 *  5. Hand sorting (sort 5 cards by rank, then suit)
 * ═══════════════════════════════════════════════════════ */

/* Method A: insertion sort (branch-heavy) */
__attribute__((noinline))
void sort_hand_insertion(Hand h) {
    for (int i = 1; i < 5; ++i) {
        Card key = h[i]; int j = i - 1;
        uint8_t kr = info_rank(key.info);
        while (j >= 0 && info_rank(h[j].info) > kr) {
            h[j+1] = h[j]; --j;
        }
        h[j+1] = key;
    }
}

/* Method B: sorting network (branchless swap) — optimal for N=5 */
#define SWAP_IF(a,b) do {                          \
    uint8_t ra = info_rank((a).info);              \
    uint8_t rb = info_rank((b).info);              \
    if (ra > rb) { Card t = (a); (a) = (b); (b) = t; } \
} while(0)

__attribute__((noinline))
void sort_hand_network(Hand h) {
    /* Optimal 9-comparator sorting network for N=5 */
    SWAP_IF(h[0],h[1]); SWAP_IF(h[3],h[4]);
    SWAP_IF(h[2],h[4]); SWAP_IF(h[2],h[3]);
    SWAP_IF(h[0],h[3]); SWAP_IF(h[0],h[2]);
    SWAP_IF(h[1],h[4]); SWAP_IF(h[1],h[3]);
    SWAP_IF(h[1],h[2]);
}

/* ═══════════════════════════════════════════════════════
 *  6. Full hand evaluation
 *     Returns: 0=highcard 1=pair 2=twopair 3=trips
 *              4=straight 5=flush 6=fullhouse
 *              7=quads 8=strflush 9=royal
 * ═══════════════════════════════════════════════════════ */
__attribute__((noinline))
int eval_hand(const Hand h) {
    uint8_t ranks[5], suits[5];
    uint8_t rank_cnt[13] = {0};
    uint16_t suit_bits = 0;

    for (int i = 0; i < 5; ++i) {
        ranks[i] = info_rank(h[i].info);
        suits[i] = info_suit(h[i].info);
        rank_cnt[ranks[i]]++;
        suit_bits |= (uint16_t)(1u << suits[i]);
    }

    bool is_flush    = (suit_bits & (suit_bits - 1)) == 0; /* only 1 bit set */
    bool is_straight = straight_bitmask(h);

    if (is_straight && is_flush) {
        /* check royal: T J Q K A */
        uint16_t bits = 0;
        for (int i = 0; i < 5; ++i) bits |= (uint16_t)(1u << ranks[i]);
        return (bits == 0x1F00u) ? 9 : 8; /* royal : straight flush */
    }
    if (is_flush)    return 5;
    if (is_straight) return 4;

    int pairs = 0, trips = 0, quads = 0;
    for (int r = 0; r < 13; ++r) {
        if (rank_cnt[r] == 4) quads++;
        else if (rank_cnt[r] == 3) trips++;
        else if (rank_cnt[r] == 2) pairs++;
    }
    if (quads)               return 7;
    if (trips && pairs)      return 6;
    if (trips)               return 3;
    if (pairs == 2)          return 2;
    if (pairs == 1)          return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════
 *  7. Deck shuffle — Fisher-Yates
 * ═══════════════════════════════════════════════════════ */
static uint64_t rng_state = 0xdeadbeefcafebabe;
static inline uint32_t fast_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)rng_state;
}

__attribute__((noinline))
void shuffle_deck(Card *deck, int n) {
    for (int i = n - 1; i > 0; --i) {
        int j = fast_rand() % (i + 1);
        Card tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
    }
}

/* ─── card equality — from previous bench ─── */
__attribute__((noinline))
bool eq_64(const Card *a, const Card *b) {
    return card_word(a) == card_word(b);
}
__attribute__((noinline))
bool eq_fields(const Card *a, const Card *b) {
    return a->next == b->next && a->info == b->info
        && a->title[0] == b->title[0]
        && a->title[1] == b->title[1]
        && a->title[2] == b->title[2];
}

/* ═══════════════════════════════════════════════════════
 *  Build synthetic deck / hands
 * ═══════════════════════════════════════════════════════ */
#define DECK_SZ  52
#define N_HANDS  (1 << 22)   /* 4 M hands */
#define N_CARDS  (1 << 24)   /* 16 M cards */
#define REPS     5

static const char *RANK_STR[] = {"2","3","4","5","6","7","8","9","T","J","Q","K","A"};
static const char  SUIT_CHAR[] = "CDHS";

static Card make_card(uint8_t rank, uint8_t suit, uint32_t next_idx) {
    Card c;
    c.next    = next_idx;
    c.info    = make_info(rank, suit);
    c.title[0]= RANK_STR[rank][0];
    c.title[1]= SUIT_CHAR[suit];
    c.title[2]= '\0';
    return c;
}

static void build_52(Card deck[DECK_SZ]) {
    int idx = 0;
    for (int s = 0; s < 4; ++s)
        for (int r = 0; r < 13; ++r)
            deck[idx] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)(idx+1 < 52 ? idx+1 : 0)),
            idx++;
}

/* flat array of random hands */
static Hand *hands_pool;
static Card *cards_pool;

static void build_pools(void) {
    hands_pool = aligned_alloc(64, N_HANDS * sizeof(Hand));
    cards_pool = aligned_alloc(64, N_CARDS * sizeof(Card));
    if (!hands_pool || !cards_pool) { perror("alloc"); exit(1); }

    Card deck[DECK_SZ];
    build_52(deck);

    /* fill hands by dealing from shuffled decks */
    for (int h = 0; h < N_HANDS; ++h) {
        shuffle_deck(deck, DECK_SZ);
        for (int c = 0; c < 5; ++c) hands_pool[h][c] = deck[c];
    }

    /* flat card pool: mix of duplicates + randoms */
    for (int i = 0; i < N_CARDS; ++i) {
        int r = fast_rand() % 13;
        int s = fast_rand() % 4;
        cards_pool[i] = make_card((uint8_t)r, (uint8_t)s, (uint32_t)i);
        if (i > 0 && (i & 1)) cards_pool[i] = cards_pool[i-1]; /* 50% dup */
    }
}

/* ═══════════════════════════════════════════════════════
 *  Timer macro
 * ═══════════════════════════════════════════════════════ */
#define TIME_IT(label, iters, code)                                       \
do {                                                                      \
    double _best = 1e9; uint64_t _chk = 0;                               \
    for (int _rep = 0; _rep < REPS; ++_rep) {                            \
        double _t0 = now_sec();                                           \
        for (int _i = 0; _i < (iters); ++_i) { code; }                  \
        double _dt = now_sec() - _t0;                                    \
        if (_dt < _best) _best = _dt;                                    \
    }                                                                     \
    double _mops = (iters) / _best / 1e6;                                \
    printf("  %-38s  %6.1f Mop/s  (%.4f s)  chk=%llu\n",                \
           label, _mops, _best, (unsigned long long)_chk);               \
    (void)_chk;                                                           \
} while(0)

/* ═══════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════ */
int main(void)
{
    printf("sizeof(Card)=%zu  N_CARDS=%dM  N_HANDS=%dM  REPS=%d\n\n",
           sizeof(Card), N_CARDS>>20, N_HANDS>>20, REPS);

    build_rank_lut();
    build_pools();

    /* ── 1. Equality ────────────────────────────────────── */
    printf("━━━ 1. Card Equality (%dM card pairs) ━━━\n", N_CARDS>>20);
    TIME_IT("eq_64  (1×64-bit cmpq)",     N_CARDS-1,
            _chk += eq_64(&cards_pool[_i], &cards_pool[_i+1]));
    TIME_IT("eq_fields (field-by-field)", N_CARDS-1,
            _chk += eq_fields(&cards_pool[_i], &cards_pool[_i+1]));

    /* ── 2. Rank extraction ─────────────────────────────── */
    printf("\n━━━ 2. Rank Extraction (%dM cards) ━━━\n", N_CARDS>>20);
    TIME_IT("rank_bitshift  (info>>2 & 0xF)", N_CARDS,
            _chk += bench_rank_bitshift(cards_pool + _i, 1));
    TIME_IT("rank_lut       (lut[info])",      N_CARDS,
            _chk += bench_rank_lut(cards_pool + _i, 1));

    /* ── 3. Suit extraction ─────────────────────────────── */
    printf("\n━━━ 3. Suit Extraction (%dM cards) ━━━\n", N_CARDS>>20);
    TIME_IT("suit_mask  (info & 0x03)",     N_CARDS,
            _chk += bench_suit_mask(cards_pool + _i, 1));

    /* ── 4. Flush detection ─────────────────────────────── */
    printf("\n━━━ 4. Flush Detection (%dM hands) ━━━\n", N_HANDS>>20);
    TIME_IT("flush_field  (5× suit compare)", N_HANDS,
            _chk += flush_field(hands_pool[_i]));
    TIME_IT("flush_word   (word-mask OR/AND)", N_HANDS,
            _chk += flush_word(hands_pool[_i]));

    /* ── 5. Straight detection ──────────────────────────── */
    printf("\n━━━ 5. Straight Detection (%dM hands) ━━━\n", N_HANDS>>20);
    TIME_IT("straight_sort    (sort+gap check)", N_HANDS,
            _chk += straight_sort(hands_pool[_i]));
    TIME_IT("straight_bitmask (rank bits fold)", N_HANDS,
            _chk += straight_bitmask(hands_pool[_i]));

    /* ── 6. Hand sorting ────────────────────────────────── */
    printf("\n━━━ 6. Hand Sorting 5 cards (%dM hands) ━━━\n", N_HANDS>>20);
    /* work on a copy so we don't ruin the pool for later */
    Hand scratch;
    TIME_IT("insertion_sort  (branch-heavy)", N_HANDS,
            memcpy(&scratch, &hands_pool[_i], sizeof(Hand));
            sort_hand_insertion(scratch);
            _chk += info_rank(scratch[0].info));
    TIME_IT("network_sort    (9 comparators)", N_HANDS,
            memcpy(&scratch, &hands_pool[_i], sizeof(Hand));
            sort_hand_network(scratch);
            _chk += info_rank(scratch[0].info));

    /* ── 7. Full hand evaluation ────────────────────────── */
    printf("\n━━━ 7. Full Hand Evaluation (%dM hands) ━━━\n", N_HANDS>>20);
    TIME_IT("eval_hand  (flush+straight+counts)", N_HANDS,
            _chk += (uint32_t)eval_hand(hands_pool[_i]));

    /* ── 8. Deck shuffle ────────────────────────────────── */
    #define N_SHUFFLES 500000
    printf("\n━━━ 8. Deck Shuffle (%dk shuffles of 52 cards) ━━━\n", N_SHUFFLES/1000);
    Card sdeck[DECK_SZ];
    build_52(sdeck);
    TIME_IT("fisher_yates  (52-card deck)", N_SHUFFLES,
            shuffle_deck(sdeck, DECK_SZ);
            _chk += sdeck[0].info);

    /* ── Hand distribution from eval ───────────────────── */
    printf("\n━━━ Hand distribution (from %dM dealt hands) ━━━\n", N_HANDS>>20);
    const char *hand_names[] = {
        "High card","Pair","Two pair","Trips",
        "Straight","Flush","Full house","Quads",
        "Str. flush","Royal flush"
    };
    uint32_t counts[10] = {0};
    for (int i = 0; i < N_HANDS; ++i) counts[eval_hand(hands_pool[i])]++;
    for (int i = 0; i < 10; ++i)
        printf("  %-14s  %8u  (%5.2f%%)\n",
               hand_names[i], counts[i], counts[i]*100.0/N_HANDS);

    free(hands_pool);
    free(cards_pool);
    return 0;
}
