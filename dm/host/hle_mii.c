// Mii service layer (nn::mii::Database). On hardware Database talks to the mii:u service (an IDatabaseService the SDK keeps at Database +0); the host has none. Its database is the player's
// console database when they supply it (mii_db_load: the mii service's system save, read-only), else an empty one.
// The CharInfo the host builds (0x58 bytes), for an amiibo's owner, is the system's default Mii as the mii service's StoreData::BuildDefault makes it: the 3DS-era default parameters, with
// the colours converted to the Switch's common colour indices (hair 1, eyes 8, mouth 19, glasses 8), a create ID of UUID version 4 and the nickname "no name"; every field is within
// CharInfoRaw::Verify's ranges (SDK 4.4.0 sdk+0x1b68c8), so CharInfoAccessor::IsValid accepts it.
#include "hle.h"

#define MII_RES_NOT_FOUND 0x87Eu        // nn::mii module 126, description 4

typedef struct __attribute__((packed)) {
    uint8_t create_id[16]; uint16_t nickname[11];
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glass_type, glass_color, glass_scale, glass_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y, reserved;
} mii_charinfo;
_Static_assert(sizeof(mii_charinfo) == 0x58, "nn::mii::CharInfo is 0x58 bytes");

// The default Mii, for an amiibo's owner (hle_sdk16.c). Its create ID is fixed, so a game that saved the Mii finds the same one again
void dm_mii_default_charinfo(void *out) {
    mii_charinfo *c = out; memset(c, 0, sizeof *c);
    uint64_t a = 0x4d49494e44454641ull, b = 0x554c54444d484f53ull;
    memcpy(c->create_id, &a, 8); memcpy(c->create_id + 8, &b, 8);
    c->create_id[6] = (uint8_t)((c->create_id[6] & 0x0f) | 0x40); c->create_id[8] = (uint8_t)((c->create_id[8] & 0x3f) | 0x80);
    static const char name[] = "no name";
    for (size_t i = 0; i < sizeof name - 1; i++) c->nickname[i] = (uint16_t)name[i];
    c->height = 64; c->build = 64;
    c->hair_type = 33; c->hair_color = 1;
    c->eye_type = 2; c->eye_color = 8; c->eye_scale = 4; c->eye_aspect = 3; c->eye_rotate = 4; c->eye_x = 2; c->eye_y = 12;
    c->eyebrow_type = 6; c->eyebrow_color = 1; c->eyebrow_scale = 4; c->eyebrow_aspect = 3; c->eyebrow_rotate = 6; c->eyebrow_x = 2; c->eyebrow_y = 10;
    c->nose_type = 1; c->nose_scale = 4; c->nose_y = 9;
    c->mouth_type = 23; c->mouth_color = 19; c->mouth_scale = 4; c->mouth_aspect = 3; c->mouth_y = 13;
    c->beard_color = 8; c->mustache_scale = 4; c->mustache_y = 10;
    c->glass_color = 8; c->glass_scale = 4; c->glass_y = 10;
    c->mole_scale = 4; c->mole_x = 2; c->mole_y = 20;
}

// The console's Mii database, from the player's own console: <DM_SYSTEM_DIR>/save/8000000000000030/MiiDatabase.dat, the mii service's system save as the Switch and the Switch 2 keep it
// (Switch 2 program 0400000000000039 mounts that save as "mii" and reads mii:/MiiDatabase.dat). The file is 0x1a98 bytes: "NFDB", 100 StoreData of 0x44 bytes, the version (1) at +0x1a94,
// the count at +0x1a95 and a CRC-16/CCITT (polynomial 0x1021, initial 0) of the bytes before it, big-endian, at +0x1a96. A StoreData is the CoreData (seven 32-bit words of bit fields and a
// 10-character nickname), the create ID and two CRCs; mii_from_store unpacks it into the CharInfo the SDK's Get returns, as the mii service does (its stored eyebrow y is 3 below the
// CharInfo's). Read once, never written
enum { MII_DB_SIZE = 0x1a98, MII_DB_MAX = 100, MII_STORE = 0x44 };
static uint16_t mii_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(p[i] << 8); for (int b = 0; b < 8; b++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1); }
    return c;
}
static unsigned mii_bits(const uint8_t *w, int word, int at, int len) { uint32_t x; memcpy(&x, w + 4 * word, 4); return (x >> at) & ((1u << len) - 1); }
static void mii_from_store(mii_charinfo *c, const uint8_t *sd) {
    memset(c, 0, sizeof *c);
    memcpy(c->create_id, sd + 0x30, 16); memcpy(c->nickname, sd + 0x1c, 20);
    c->hair_type = mii_bits(sd, 0, 0, 8); c->height = mii_bits(sd, 0, 8, 7); c->mole_type = mii_bits(sd, 0, 15, 1); c->build = mii_bits(sd, 0, 16, 7);
    c->hair_flip = mii_bits(sd, 0, 23, 1); c->hair_color = mii_bits(sd, 0, 24, 7); c->type = mii_bits(sd, 0, 31, 1);
    c->eye_color = mii_bits(sd, 1, 0, 7); c->gender = mii_bits(sd, 1, 7, 1); c->eyebrow_color = mii_bits(sd, 1, 8, 7); c->mouth_color = mii_bits(sd, 1, 16, 7); c->beard_color = mii_bits(sd, 1, 24, 7);
    c->glass_color = mii_bits(sd, 2, 0, 7); c->eye_type = mii_bits(sd, 2, 8, 6); c->region_move = mii_bits(sd, 2, 14, 2); c->mouth_type = mii_bits(sd, 2, 16, 6);
    c->font_region = mii_bits(sd, 2, 22, 2); c->eye_y = mii_bits(sd, 2, 24, 5); c->glass_scale = mii_bits(sd, 2, 29, 3);
    c->eyebrow_type = mii_bits(sd, 3, 0, 5); c->mustache_type = mii_bits(sd, 3, 5, 3); c->nose_type = mii_bits(sd, 3, 8, 5); c->beard_type = mii_bits(sd, 3, 13, 3);
    c->nose_y = mii_bits(sd, 3, 16, 5); c->mouth_aspect = mii_bits(sd, 3, 21, 3); c->mouth_y = mii_bits(sd, 3, 24, 5); c->eyebrow_aspect = mii_bits(sd, 3, 29, 3);
    c->mustache_y = mii_bits(sd, 4, 0, 5); c->eye_rotate = mii_bits(sd, 4, 5, 3); c->glass_y = mii_bits(sd, 4, 8, 5); c->eye_aspect = mii_bits(sd, 4, 13, 3);
    c->mole_x = mii_bits(sd, 4, 16, 5); c->eye_scale = mii_bits(sd, 4, 21, 3); c->mole_y = mii_bits(sd, 4, 24, 5);
    c->glass_type = mii_bits(sd, 5, 0, 5); c->favorite_color = mii_bits(sd, 5, 8, 4); c->faceline_type = mii_bits(sd, 5, 12, 4); c->faceline_color = mii_bits(sd, 5, 16, 4);
    c->faceline_wrinkle = mii_bits(sd, 5, 20, 4); c->faceline_make = mii_bits(sd, 5, 24, 4); c->eye_x = mii_bits(sd, 5, 28, 4);
    c->eyebrow_scale = mii_bits(sd, 6, 0, 4); c->eyebrow_rotate = mii_bits(sd, 6, 4, 4); c->eyebrow_x = mii_bits(sd, 6, 8, 4); c->eyebrow_y = (uint8_t)(mii_bits(sd, 6, 12, 4) + 3);
    c->nose_scale = mii_bits(sd, 6, 16, 4); c->mouth_scale = mii_bits(sd, 6, 20, 4); c->mustache_scale = mii_bits(sd, 6, 24, 4); c->mole_scale = mii_bits(sd, 6, 28, 4);
}
// The database the service answers from: the console's Miis, up to MII_DB_MAX
static mii_charinfo g_mii_db[MII_DB_MAX];
static int g_mii_n;
// <0: the file is not a database (logged); else the Miis read
static int mii_db_parse(const uint8_t *b, size_t size, mii_charinfo *out) {
    if (size != MII_DB_SIZE || memcmp(b, "NFDB", 4)) return -1;
    if (b[0x1a94] != 1 || b[0x1a95] > MII_DB_MAX) return -2;
    if (mii_crc16(b, 0x1a96) != (uint16_t)(b[0x1a96] << 8 | b[0x1a97])) return -3;
    for (int i = 0; i < b[0x1a95]; i++) mii_from_store(&out[i], b + 4 + (size_t)i * MII_STORE);
    return b[0x1a95];
}
const char *dm_system_dir(char *buf, size_t n);   // hle_fs.c: $DM_SYSTEM_DIR, default $DM_ROOT/run/platform/system
static void mii_db_load(void) {
    char t[1024], p[1100]; const char *sys = dm_system_dir(t, sizeof t); g_mii_n = 0;
    snprintf(p, sizeof p, "%s/save/8000000000000030/MiiDatabase.dat", sys);
    FILE *f = fopen(p, "rb"); if (!f) return;
    uint8_t *b = malloc(MII_DB_SIZE + 1); size_t n = b ? fread(b, 1, MII_DB_SIZE + 1, f) : 0; fclose(f);
    int r = b ? mii_db_parse(b, n, g_mii_db) : -1; free(b);
    static const char *why[] = {"", "not a Mii database (size or NFDB)", "an unknown version or count", "its CRC does not match"};
    if (r < 0) LOG("[RED] mii: %s is %s: the database stays empty", p, why[-r]);
    else { g_mii_n = r; LOG("mii: %d Miis of the console's database from %s", r, p); }
}
static void mii_db_once(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, mii_db_load); }

// The object keeps its service pointer at +0, which the constructor zeroes and the destructor releases through its vtable when set; the host never sets it
HLE_FORCE("_ZN2nn3mii8Database10InitializeEv", "mii:u service; the host's database: the console's own when supplied, else empty", Result, h_mii_Init, void *self) {
    (void)self; mii_db_once();
    LOG("mii: Database::Initialize (%d Miis of the console's database)", g_mii_n); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database8FinalizeEv", "mii:u service; nothing to release", void, h_mii_Fin, void *self) { (void)self; }
// A Mii is found by its create ID (CharInfo +0)
static int mii_find(const void *info) { for (int i = 0; info && i < g_mii_n; i++) if (!memcmp(g_mii_db[i].create_id, info, 16)) return i; return -1; }
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "mii:u service; the Mii's place in the console's database", Result, h_mii_Index, const void *self, int *out, const void *info) {
    (void)self; *out = mii_find(info); return *out < 0 ? MII_RES_NOT_FOUND : RES_OK;
}
