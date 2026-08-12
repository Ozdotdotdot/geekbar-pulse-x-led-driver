#pragma once

#include <cstddef>
#include <cstdint>

// Named LED index groups, derived from led_map.json / led_map_overlay.png.
// Each group is the set of physically connected constellation nodes (the
// ones joined by printed lines on the flex film), not every star in that
// region -- unconnected background stars are left out.

static const uint8_t CONSTELLATION_LEFT[] = {
    68, 69, 70, 71, 72, 82, 83, 84, 88, 89, 90, 91, 96, 97, 98, 99, 100,
    103, 106, 107,
};
static const size_t CONSTELLATION_LEFT_LEN =
    sizeof(CONSTELLATION_LEFT) / sizeof(CONSTELLATION_LEFT[0]);

// Same nodes as CONSTELLATION_LEFT, paired with each node's depth (edge
// count from root 72) in the branching tree below, for wavefront-style
// animations where every branch at a given depth lights simultaneously:
//
//   72-71-70 -+- 69-68
//             +- 82 -+- 83-84
//                    +- 89 -+- 88
//                           +- 91
//                           +- 90-96-97-98-99-100-103-106-107
static const uint8_t CONSTELLATION_LEFT_TREE[] = {
    72, 71, 70, 69, 68, 82, 83, 84, 89, 88, 91, 90, 96, 97, 98, 99, 100,
    103, 106, 107,
};
static const uint8_t CONSTELLATION_LEFT_DEPTH[] = {
    0, 1, 2, 3, 4, 3, 4, 5, 4, 5, 5, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};
static const size_t CONSTELLATION_LEFT_TREE_LEN =
    sizeof(CONSTELLATION_LEFT_TREE) / sizeof(CONSTELLATION_LEFT_TREE[0]);

// Undirected edge list for the same tree, for pulse-simulation animations
// that need real graph adjacency (who's a leaf, who's a junction) rather
// than just depth-from-root.
static const uint8_t CONSTELLATION_LEFT_EDGES[][2] = {
    {72, 71}, {71, 70}, {70, 69}, {69, 68}, {70, 82}, {82, 83}, {83, 84},
    {82, 89}, {89, 88}, {89, 91}, {89, 90}, {90, 96}, {96, 97}, {97, 98},
    {98, 99}, {99, 100}, {100, 103}, {103, 106}, {106, 107},
};
static const size_t CONSTELLATION_LEFT_EDGES_LEN =
    sizeof(CONSTELLATION_LEFT_EDGES) / sizeof(CONSTELLATION_LEFT_EDGES[0]);

// Right constellation, rooted at 125:
//   125-126-127 -+- 136-129-130-131-132
//                +- 133-134-135
// (122, 124, 137, 139 are unconnected background stars nearby, and both
// "128" and all three "138" LEDs are tight decorative clusters off the
// line path, not path nodes -- excluded here.)
static const uint8_t CONSTELLATION_RIGHT[] = {
    125, 126, 127, 133, 134, 135, 136, 129, 130, 131, 132,
};
static const size_t CONSTELLATION_RIGHT_LEN =
    sizeof(CONSTELLATION_RIGHT) / sizeof(CONSTELLATION_RIGHT[0]);

static const uint8_t CONSTELLATION_RIGHT_TREE[] = {
    125, 126, 127, 136, 133, 129, 134, 130, 135, 131, 132,
};
static const uint8_t CONSTELLATION_RIGHT_DEPTH[] = {
    0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 7,
};
static const size_t CONSTELLATION_RIGHT_TREE_LEN =
    sizeof(CONSTELLATION_RIGHT_TREE) / sizeof(CONSTELLATION_RIGHT_TREE[0]);

static const uint8_t CONSTELLATION_RIGHT_EDGES[][2] = {
    {125, 126}, {126, 127}, {127, 136}, {127, 133}, {136, 129},
    {133, 134}, {129, 130}, {134, 135}, {130, 131}, {131, 132},
};
static const size_t CONSTELLATION_RIGHT_EDGES_LEN =
    sizeof(CONSTELLATION_RIGHT_EDGES) / sizeof(CONSTELLATION_RIGHT_EDGES[0]);

// Left and right trees concatenated into one node/depth list. Wavefront
// animations only ever check "is this node at the current depth" -- they
// don't care whether same-depth nodes are actually connected -- so this
// makes both trees animate in perfect depth-sync as if they were one,
// even though they're two physically separate branches. The right tree
// (max depth 7) simply finishes first and holds while the left tree
// (max depth 13) keeps going.
static const uint8_t BOTH_CONSTELLATIONS_TREE[] = {
    72, 71, 70, 69, 68, 82, 83, 84, 89, 88, 91, 90, 96, 97, 98, 99, 100,
    103, 106, 107,
    125, 126, 127, 136, 133, 129, 134, 130, 135, 131, 132,
};
static const uint8_t BOTH_CONSTELLATIONS_DEPTH[] = {
    0, 1, 2, 3, 4, 3, 4, 5, 4, 5, 5, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 7,
};
static const size_t BOTH_CONSTELLATIONS_TREE_LEN =
    sizeof(BOTH_CONSTELLATIONS_TREE) / sizeof(BOTH_CONSTELLATIONS_TREE[0]);

// Every remaining LED that isn't part of a constellation path, a digit
// cluster (0-15, 40-55), the ring/X icon (16-39, 56-59), the lightning
// bolt icon (74, 76), the juice-drop icon (60, 61, 112, 113), or a
// multi-LED-per-index decorative dot (128, 138) -- genuine unconnected
// background stars with no printed line to anything, scattered across
// the rest of the film.
static const uint8_t STRAY_STARS[] = {
    62,  63,  64,  65,  66,  67,  73,  75,  77,  78,  79,  80,  81,  85,
    86,  87,  92,  93,  94,  95,  101, 102, 104, 105, 108, 109, 110, 111,
    114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 137, 139, 140,
    141, 142, 143,
};
static const size_t STRAY_STARS_LEN =
    sizeof(STRAY_STARS) / sizeof(STRAY_STARS[0]);

// Every LED on the film, 0-143, for animations that should run across the
// whole panel at once (e.g. twinkle) rather than one named region.
static const uint8_t ALL_LEDS[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,
    56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
    70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,
    84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,
    98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
    140, 141, 142, 143,
};
static const size_t ALL_LEDS_LEN = sizeof(ALL_LEDS) / sizeof(ALL_LEDS[0]);

// Both constellation paths combined, for animations that treat left+right
// as a single static group (e.g. holding them lit while stray_stars
// twinkles alongside).
static const uint8_t BOTH_CONSTELLATIONS[] = {
    68,  69,  70,  71,  72,  82,  83,  84,  88,  89,  90,  91,  96,  97,
    98,  99,  100, 103, 106, 107, 125, 126, 127, 133, 134, 135, 136, 129,
    130, 131, 132,
};
static const size_t BOTH_CONSTELLATIONS_LEN =
    sizeof(BOTH_CONSTELLATIONS) / sizeof(BOTH_CONSTELLATIONS[0]);
