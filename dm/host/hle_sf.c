// Services whose client the SDK links in (nn::sf::hipc): host sessions, each request answered in its buffer for sf_served (success, zeroed output); others refused.
// Layout (Atmosphère / libnx): HIPC header, special header, descriptors, then the CMIF part 16-byte aligned: domain header, "SFCI" header, arguments. Replies: "SFCO".
#include "hle.h"
#include <pthread.h>

#define SF_H0 0xd0000u   // outside the kernel handle table (0x1000 + slot, 4096 slots)
#define SF_MAX 64
#define SF_RES_BAD_HANDLE 0xe401u   // kernel ResultInvalidHandle
static struct { char name[16]; int used, domain; unsigned seen; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;
// The SDK aborts on any failed notif:a request; zeroed output means no alarms. caps: no album.
static const char *const sf_served[] = {"notif:a", "caps:su", "caps:u"};

static int sf_slot(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static uint32_t sf_new(const char *name, int domain) {
    pthread_mutex_lock(&g_sf_mu);
    unsigned i = 0;
    while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) die("sf: more than %d host service sessions", SF_MAX);
    strlcpy(g_sf[i].name, name, sizeof g_sf[i].name);
    g_sf[i].used = 1;
    g_sf[i].domain = domain;
    g_sf[i].seen = 0;
    pthread_mutex_unlock(&g_sf_mu);
    return SF_H0 + i;
}

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves the names it serves itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a service through sm → a host session for the services the host serves", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    for (size_t k = 0; k < sizeof sf_served / sizeof *sf_served; k++)
        if (!strncmp(name, sf_served[k], 8)) {
            *out = sf_new(sf_served[k], 0);
            LOG("sf: %s → host session %#x (every request succeeds with zeroed output)", sf_served[k], *out);
            return RES_OK;
        }
    LOG("[RED] sf: the game connects to service %.8s itself, which the host does not serve", name);
    return 0xe15u;   // sm ResultNotRegistered
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a session (kernel handle) → the host session", void, h_sf_Close, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) {
        LOG("[RED] sf: CloseClientSessionHandle on %#x, not a host session", h);
        return;
    }
    pthread_mutex_lock(&g_sf_mu);
    g_sf[i].used = 0;
    pthread_mutex_unlock(&g_sf_mu);
}
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "control request: pointer buffer size of a session → 0 for a host session", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    if (sf_slot(h) < 0) {
        LOG("[RED] sf: QueryPointerBufferSize on %#x, not a host session", h);
        return RES_FAIL;
    }
    *out = 0;
    return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "control request: convert a session to a domain → the host session's object 1", Result, h_sf_ToDomain,
          uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) {
        LOG("[RED] sf: ConvertSessionToDomain on %#x, not a host session", h);
        return RES_FAIL;
    }
    g_sf[i].domain = 1;
    *out = 1;
    return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "control request: clone a session → a new host session to the same service", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) {
        LOG("[RED] sf: DuplicateSession on %#x, not a host session", h);
        return RES_FAIL;
    }
    *out = sf_new(g_sf[i].name, g_sf[i].domain);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) → a new host session to the same service", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

// Only the CMIF part is read: no served service looks at handles, buffers or arguments.
HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) → answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    int i = sf_slot(h);
    if (i < 0 || size < 0x40) {
        LOG("[RED] sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size);
        return SF_RES_BAD_HANDLE;
    }
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, na = m[0] >> 20 & 15, nb = m[0] >> 24 & 15, nw = m[0] >> 28 & 15, ndw = m[1] & 0x3ff;
    uint32_t *p = m + 2;
    if (m[1] >> 31) {   // special header
        uint32_t sh = *p++;
        p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15);
    }
    p += 2 * nx + 3 * (na + nb + nw);
    size_t data = (size_t)((uint8_t *)p - base), cm = (data + 15) & ~(size_t)15;
    int dom = g_sf[i].domain && (type == 4 || type == 6);
    unsigned dtype = dom ? base[cm] : 1, obj = dom ? *(uint32_t *)(base + cm + 4) : 0;
    size_t hin = cm + (dom ? 16 : 0);
    if (hin + 16 > size || data + ndw * 4 > size) {
        LOG("[RED] sf: %s: request of %u data words does not fit %llu bytes", g_sf[i].name, ndw, (unsigned long long)size);
        return SF_RES_BAD_HANDLE;
    }
    uint32_t magic = *(uint32_t *)(base + hin), ver = *(uint32_t *)(base + hin + 4), cmd = *(uint32_t *)(base + hin + 8);
    static int trace = -1;
    if (trace < 0) trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE"));
    if (trace) {
        char hx[3 * 0x80 + 1] = "";
        for (unsigned k = 0; k < 0x80 && k < size; k++) snprintf(hx + 3 * k, 4, "%02x ", base[k]);
        LOG("sf: %s request: %s", g_sf[i].name, hx);
    }
    if (type == 2) return 0xf601u;   // HIPC Close: the closed-session result
    if (type != 4 && type != 6) {
        LOG("[RED] sf: %s: HIPC message type %u not served", g_sf[i].name, type);
        return SF_RES_BAD_HANDLE;
    }
    if (dtype == 2) {   // a domain object closed
        memset(base, 0, 16);
        return RES_OK;
    }
    if (magic != 0x49434653) {   // "SFCI"
        LOG("[RED] sf: %s: request without SFCI (%#x)", g_sf[i].name, magic);
        return SF_RES_BAD_HANDLE;
    }
    unsigned key = (obj & 0xff) << 16 | (cmd & 0xffff);
    pthread_mutex_lock(&g_sf_mu);
    int first = g_sf[i].seen != key;
    g_sf[i].seen = key;
    pthread_mutex_unlock(&g_sf_mu);
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): success, zeroed output", g_sf[i].name, obj, cmd, ndw);
    // No descriptors, no special header; the rest of the buffer reads as zero.
    size_t end = size < 0x1000 ? size : 0x1000;
    memset(base, 0, end);
    m[1] = (uint32_t)((end - 8) / 4 < 0x3ff ? (end - 8) / 4 : 0x3ff);
    uint32_t *o = (uint32_t *)(base + 16 + (dom ? 16 : 0));
    o[0] = 0x4f434653;   // "SFCO"
    o[1] = ver;
    o[2] = RES_OK;
    o[3] = 0;
    return RES_OK;
}

void dm_sf_reset(void) {
    pthread_mutex_lock(&g_sf_mu);
    memset(g_sf, 0, sizeof g_sf);
    pthread_mutex_unlock(&g_sf_mu);
}
