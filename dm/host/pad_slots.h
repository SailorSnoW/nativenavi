// Player slots (pure, for tests): a connecting controller takes a free slot its name last held, else player 1, else the lowest never-held
// slot, else the lowest free; -1 when all are held. GameController gives no identity across reconnects: two of one model may trade places.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dm_input.h"

typedef struct { const void *owner[DM_PLAYERS]; char last[DM_PLAYERS][64]; } pad_slots;

static int pad_slot_of(const pad_slots *s, const void *owner) {
    for (int i = 0; owner && i < DM_PLAYERS; i++) if (s->owner[i] == owner) return i;
    return -1;
}
static int pad_slot_hold(pad_slots *s, int p, const void *owner, const char *name) {
    s->owner[p] = owner;
    snprintf(s->last[p], sizeof s->last[p], "%s", name ? name : "");
    return p;
}
static int pad_slot_take(pad_slots *s, const void *owner, const char *name) {
    int p = pad_slot_of(s, owner);
    if (p >= 0 || !owner) return p;
    if (!name) name = "";
    for (int i = 0; p < 0 && *name && i < DM_PLAYERS; i++) if (!s->owner[i] && !strcmp(s->last[i], name)) p = i;
    if (p < 0 && !s->owner[0]) p = 0;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i] && !s->last[i][0]) p = i;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i]) p = i;
    if (p < 0) return -1;
    return pad_slot_hold(s, p, owner, name);
}
// whatever slot its name held before
static int pad_slot_take_lowest(pad_slots *s, const void *owner, const char *name) {
    int p = pad_slot_of(s, owner);
    if (p >= 0 || !owner) return p;
    for (int i = 0; i < DM_PLAYERS; i++) if (!s->owner[i]) return pad_slot_hold(s, i, owner, name);
    return -1;
}
static int pad_slot_free(pad_slots *s, const void *owner) {
    int p = pad_slot_of(s, owner);
    if (p >= 0) s->owner[p] = NULL;
    return p;
}
static uint32_t pad_slot_mask(const pad_slots *s) {   // bit p: slot p held
    uint32_t m = 0;
    for (int i = 0; i < DM_PLAYERS; i++) if (s->owner[i]) m |= 1u << i;
    return m;
}
