// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (caps:u; caps:su with a process ID, arguments and an input buffer; notif:a as a domain) succeeds
// with zeroed output and no special header, a duplicate answers as its session does, and a name the host does not serve is refused as sm refuses one that is not registered
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// A CMIF request for command cmd on a non-domain session (HIPC type 4, no special header, no descriptors): the data words from +8, "SFCI" at +0x10
static Result request(uint32_t h, uint32_t *m, unsigned cmd) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
// The same on a domain session as the SDK's notif:a client sends it: the process ID in a special header when pid, one B descriptor (the buffer buf) when buf, the domain header for
// object 1 at the first 16-byte boundary after them, then "SFCI"
static Result domain_request(uint32_t h, uint32_t *m, unsigned cmd, int pid, void *buf, uint32_t size) {
    memset(m, 0xcc, 0x100);
    m[0] = 4 | (buf ? 1u << 24 : 0); m[1] = 16 | (pid ? 1u << 31 : 0);
    uint32_t *p = m + 2;
    if (pid) { *p++ = 1; *p++ = 0; *p++ = 0; }
    if (buf) { uint64_t a = (uint64_t)(uintptr_t)buf; *p++ = size; *p++ = (uint32_t)a; *p++ = (uint32_t)(a >> 32 & 0xf) << 28 | (uint32_t)(a >> 36 & 7) << 2; }
    size_t cm = ((size_t)((uint8_t *)p - (uint8_t *)m) + 15) & ~(size_t)15;
    uint32_t *d = (uint32_t *)((uint8_t *)m + cm);
    d[0] = 1 | 16u << 16; d[1] = 1; d[2] = d[3] = 0;   // domain header: send a message to object 1, 16 bytes of data
    d[4] = 0x49434653; d[5] = 0; d[6] = cmd; d[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
int main(void) {
    uint32_t m[0x40], h = 0, unserved = 0;
    CHECK(h_sf_Connect(&h, "caps:u") == RES_OK && h, "caps:u is served");
    CHECK(request(h, m, 0) == RES_OK, "caps:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "caps:u: success, no special header, SFCO at +0x10");
    CHECK(h_sf_Connect(&unserved, "chat:u") == 0xe15u && h_sf_Connect(&unserved, "caps:a") == 0xe15u && h_sf_Connect(&unserved, "caps:sux") == 0xe15u && !unserved,
          "a service the host does not serve is not registered");

    uint32_t su = 0;
    CHECK(h_sf_Connect(&su, "caps:su") == RES_OK && su && su != h, "caps:su is served");
    if (su) {
        uint8_t empty[sizeof m - 0x20] = {0};
        // A process ID in a special header and a u64 after SFCI: 3 alignment words + 4 CMIF words + 2 input words.
        memset(m, 0xcc, sizeof m);
        m[0] = 4; m[1] = 9 | 1u << 31;
        m[2] = 1; m[3] = 0x1234; m[4] = 0;   // PID descriptor, no copied or moved handles
        m[8] = 0x49434653; m[9] = 0; m[10] = 0; m[11] = 0;
        m[12] = m[13] = 0;
        CHECK(h_sf_Send(su, m, sizeof m) == RES_OK, "a request with a PID and a u64");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "with a PID: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "with a PID: no output data");

        // 16 bytes of arguments after SFCI and a type-0x5 input buffer (one A descriptor): no output, the buffer is not written.
        uint8_t input[32], before[sizeof input];
        memset(input, 0xa5, sizeof input); memcpy(before, input, sizeof input);
        memset(m, 0xcc, sizeof m);
        uint64_t address = (uint64_t)(uintptr_t)input;
        m[0] = 4 | 1u << 20; m[1] = 11;   // 3 alignment words + 4 CMIF words + 4 argument words
        m[2] = sizeof input; m[3] = (uint32_t)address;
        m[4] = (uint32_t)(address >> 32 & 0xf) << 28 | (uint32_t)(address >> 36 & 0x3fffff) << 2;
        m[8] = 0x49434653; m[9] = 0; m[10] = 1; m[11] = 0;
        m[12] = 1; m[13] = 2; m[14] = 3; m[15] = 4;
        CHECK(h_sf_Send(su, m, sizeof m) == RES_OK, "a request with arguments and an input buffer");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "with an input buffer: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "with an input buffer: no output data");
        CHECK(!memcmp(input, before, sizeof input), "the input buffer is left untouched");
        h_sf_Close(su);
        CHECK(request(su, m, 0) == 0xe401u, "a closed session is no session");
    }

    // notif:a as nn::notification::ListAlarmSettings reaches it: connect, a domain, Initialize with the process ID, then List into the caller's array, which stays untouched
    uint32_t notif = 0, obj = 0; uint8_t alarms[8 * 0x40], before[sizeof alarms];
    memset(alarms, 0xa5, sizeof alarms); memcpy(before, alarms, sizeof alarms);
    CHECK(h_sf_Connect(&notif, "notif:a") == RES_OK && notif, "notif:a is served");
    CHECK(h_sf_ToDomain(&obj, notif) == RES_OK && obj == 1, "notif:a converts to a domain");
    CHECK(domain_request(notif, m, 1000, 1, NULL, 0) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a Initialize with the process ID: success");
    CHECK(domain_request(notif, m, 520, 0, alarms, sizeof alarms) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a ListAlarmSettings: success");
    CHECK(m[12] == 0, "no alarms listed");
    CHECK(!memcmp(alarms, before, sizeof alarms), "the caller's alarm array is not written");
    CHECK(domain_request(notif, m, 500, 0, alarms, 0x40) == RES_OK && m[10] == RES_OK && m[12] == 0, "notif:a RegisterAlarmSetting: success, ID 0");

    // A duplicate is a new session to the same service, a domain as its original; the pointer buffer of a host session is empty
    uint32_t dup = 0; uint16_t ptr = 0xffff;
    CHECK(h_sf_Dup(&dup, notif) == RES_OK && dup && dup != notif, "a duplicate of the notif:a session");
    CHECK(domain_request(dup, m, 520, 0, alarms, sizeof alarms) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK && m[12] == 0, "the duplicate answers as notif:a, in its domain");
    CHECK(h_sf_PtrBuf(&ptr, dup) == RES_OK && ptr == 0, "no pointer buffer on a host session");
    h_sf_Close(dup);
    CHECK(domain_request(notif, m, 520, 0, alarms, sizeof alarms) == RES_OK && m[10] == RES_OK, "closing the duplicate leaves the original open");
    h_sf_Close(notif); h_sf_Close(h);
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
