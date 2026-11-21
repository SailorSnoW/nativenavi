// hle_mii.c unit test: the console's Mii database (<DM_SYSTEM_DIR>/save/8000000000000030/MiiDatabase.dat). A StoreData's bit fields unpack into the CharInfo fields they hold
// (eyebrow y 3 higher), a file with a wrong CRC, size or version leaves the database empty, GetIndex finds a Mii of the database by its create ID, and the default Mii (an amiibo's
// owner) is none of them
#include "../hle_mii.c"
#include <sys/stat.h>
#include <unistd.h>
const char *dm_system_dir(char *buf, size_t n) { (void)buf; (void)n; const char *v = getenv("DM_SYSTEM_DIR"); return v ? v : "build/t_mii.none"; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); puts(""); fails++; } } while (0)
static void put_bits(uint8_t *sd, int word, int at, int len, unsigned v) {
    uint32_t x; memcpy(&x, sd + 4 * word, 4); x = (x & ~(((1u << len) - 1) << at)) | ((v & ((1u << len) - 1)) << at); memcpy(sd + 4 * word, &x, 4);
}
static void db_crc(uint8_t *b) { uint16_t c = mii_crc16(b, 0x1a96); b[0x1a96] = (uint8_t)(c >> 8); b[0x1a97] = (uint8_t)c; }

int main(void) {
    static uint8_t db[MII_DB_SIZE];
    memcpy(db, "NFDB", 4); db[0x1a94] = 1; db[0x1a95] = 2;
    uint8_t *a = db + 4, *b = db + 4 + MII_STORE;
    put_bits(a, 0, 0, 8, 131); put_bits(a, 0, 8, 7, 100); put_bits(a, 0, 24, 7, 99); put_bits(a, 0, 31, 1, 1);   // hair 131, height 100, hair colour 99, special
    put_bits(a, 1, 7, 1, 1); put_bits(a, 2, 8, 6, 59); put_bits(a, 2, 24, 5, 18); put_bits(a, 3, 0, 5, 23);      // female, eye type 59, eye y 18, eyebrow type 23
    put_bits(a, 5, 8, 4, 11); put_bits(a, 5, 28, 4, 12); put_bits(a, 6, 12, 4, 15); put_bits(a, 6, 28, 4, 8);    // favourite colour 11, eye x 12, eyebrow y 15 (stored), mole scale 8
    const char nick[] = "Jo"; for (int i = 0; nick[i]; i++) { a[0x1c + 2 * i] = (uint8_t)nick[i]; }
    for (int i = 0; i < 16; i++) { a[0x30 + i] = (uint8_t)(0xa0 + i); b[0x30 + i] = (uint8_t)(0xb0 + i); }
    db_crc(db);

    mii_charinfo c[MII_DB_MAX];
    CHECK(mii_db_parse(db, sizeof db, c) == 2, "two Miis parsed");
    CHECK(c[0].hair_type == 131 && c[0].height == 100 && c[0].hair_color == 99 && c[0].type == 1, "word 0: hair %d height %d colour %d type %d", c[0].hair_type, c[0].height, c[0].hair_color, c[0].type);
    CHECK(c[0].gender == 1 && c[0].eye_type == 59 && c[0].eye_y == 18 && c[0].eyebrow_type == 23, "words 1-3");
    CHECK(c[0].favorite_color == 11 && c[0].eye_x == 12 && c[0].mole_scale == 8, "words 5-6");
    CHECK(c[0].eyebrow_y == 18, "eyebrow y is the stored one + 3: %d", c[0].eyebrow_y);
    CHECK(c[0].nickname[0] == 'J' && c[0].nickname[1] == 'o' && c[0].nickname[2] == 0 && c[0].nickname[10] == 0, "nickname");
    CHECK(c[0].create_id[0] == 0xa0 && c[0].create_id[15] == 0xaf && c[1].create_id[0] == 0xb0, "create IDs");
    CHECK(c[0].height == 100 && c[1].height == 0, "the second Mii is its own record");

    static uint8_t bad[MII_DB_SIZE]; memcpy(bad, db, sizeof db); bad[0x100] ^= 1;
    CHECK(mii_db_parse(bad, sizeof bad, c) == -3, "a CRC mismatch is refused");
    CHECK(mii_db_parse(db, sizeof db - 1, c) == -1, "a short file is refused");
    memcpy(bad, db, sizeof db); bad[0x1a94] = 2; db_crc(bad);
    CHECK(mii_db_parse(bad, sizeof bad, c) == -2, "version 2 is refused");

    // Through the service calls, from a DM_SYSTEM_DIR in the build folder
    char root[] = "build/t_mii.XXXXXX", p[256];
    CHECK(mkdtemp(root) != NULL, "temporary folder");
    snprintf(p, sizeof p, "%s/save", root); mkdir(p, 0755); snprintf(p, sizeof p, "%s/save/8000000000000030", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/save/8000000000000030/MiiDatabase.dat", root);
    FILE *f = fopen(p, "wb"); CHECK(f && fwrite(db, 1, sizeof db, f) == sizeof db, "write the database"); if (f) fclose(f);
    setenv("DM_SYSTEM_DIR", root, 1);
    CHECK(h_mii_Init(NULL) == RES_OK && g_mii_n == 2, "Initialize reads the database: %d", g_mii_n);
    int idx = -1;
    CHECK(h_mii_Index(NULL, &idx, &c[0]) == RES_OK && idx == 0 && h_mii_Index(NULL, &idx, &c[1]) == RES_OK && idx == 1, "GetIndex by create ID");
    mii_charinfo other = c[1]; other.create_id[3] ^= 1; idx = 5;
    CHECK(h_mii_Index(NULL, &idx, &other) == MII_RES_NOT_FOUND && idx == -1, "another create ID is not in the database");
    // the default Mii: the system's parameters under a fixed create ID of UUID version 4, in no database
    mii_charinfo def, again; memset(&def, 0xff, sizeof def); dm_mii_default_charinfo(&def); dm_mii_default_charinfo(&again);
    CHECK(def.nickname[0] == 'n' && def.nickname[6] == 'e' && def.nickname[7] == 0 && def.height == 64 && def.build == 64 && def.gender == 0 && def.favorite_color == 0 && def.reserved == 0, "the default Mii's fields");
    CHECK((def.create_id[6] & 0xf0) == 0x40 && (def.create_id[8] & 0xc0) == 0x80 && !memcmp(&def, &again, sizeof def), "its create ID is a version 4 UUID, the same each time");
    CHECK(h_mii_Index(NULL, &idx, &def) == MII_RES_NOT_FOUND, "the default Mii is not in the database");
    h_mii_Fin(NULL); CHECK(h_mii_Index(NULL, &idx, &c[0]) == RES_OK && idx == 0, "Finalize releases nothing");
    // a console file that is not a database is refused and lists nothing; the good one reads again
    f = fopen(p, "r+b"); if (f) { fputc('X', f); fclose(f); }
    mii_db_load(); CHECK(g_mii_n == 0 && h_mii_Index(NULL, &idx, &c[0]) == MII_RES_NOT_FOUND, "a damaged console file is refused: %d", g_mii_n);
    f = fopen(p, "wb"); if (f) { fwrite(db, 1, sizeof db, f); fclose(f); }
    mii_db_load(); CHECK(g_mii_n == 2, "the database reads again: %d", g_mii_n);
    unlink(p); snprintf(p, sizeof p, "%s/save/8000000000000030", root); rmdir(p); snprintf(p, sizeof p, "%s/save", root); rmdir(p); rmdir(root);

    printf("t_mii: %s\n", fails ? "FAILED" : "all checks pass");
    return fails != 0;
}
