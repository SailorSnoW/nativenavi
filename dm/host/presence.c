// Discord presence: what gamedata.c reads, shown as the player's activity through Discord's local RPC, the Unix socket $TMPDIR/discord-ipc-<0..9> the Discord app
// listens on. A frame is a u32 opcode and a u32 length (little-endian), then that much JSON: 0 handshake, 1 a command or its answer, 2 close, 3 ping, 4 pong.
// Discord takes at most 5 activity updates in 20 s, so one goes out every 5 s at most; it drops the activity itself when this process's socket closes.
#include "dm.h"
#include "gamedata.h"
#include "presence.h"
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG };
#define SEND_EVERY_S 5
#define READ_EVERY_S 2
#define RETRY_S 15
#define REFUSED_RETRY_S 300   // a refused application ID does not become valid by asking again
#define SEP " · "
#define HOVER_TEXT "with NativeNavi"   // the activity's name is already the game's

// ---- the game's place names in the game's language (places.tsv: an ID, then a column per language code) ----
#define MAX_PLACES 256
static struct { char id[64], name[128]; } g_places[MAX_PLACES];
static int g_nplaces;

static int split_tabs(char *line, char **col, int max) {
    int n = 0;
    char *s = line, *t;
    line[strcspn(line, "\r\n")] = 0;
    while (n < max && (t = strsep(&s, "\t"))) col[n++] = t;
    return n;
}
static void places_load(const char *dir, const char *lang) {
    char path[1100], line[4096], *col[64];
    snprintf(path, sizeof path, "%s/places.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) { LOG("presence: no %s: places are named from their IDs", path); return; }
    int want = -1, en = -1, ncol = 0;
    char took[16] = "";
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int n = split_tabs(line, col, 64);
        if (ncol == 0) {   // the header: id, then the language codes
            ncol = n;
            size_t ll = strcspn(lang, "-");
            for (int i = 1; i < n; i++) {
                if (!strcmp(col[i], lang)) want = i;
                if (!strcmp(col[i], "en-US")) en = i;
            }
            for (int i = 1; want < 0 && i < n; i++) if (strlen(col[i]) == ll && !strncmp(col[i], lang, ll)) want = i;
            if (want < 0) want = en > 0 ? en : 1;
            if (want < n) snprintf(took, sizeof took, "%s", col[want]);
            continue;
        }
        if (n <= want || !*col[0] || !*col[want] || g_nplaces == MAX_PLACES) continue;
        snprintf(g_places[g_nplaces].id, sizeof g_places[0].id, "%s", col[0]);
        snprintf(g_places[g_nplaces].name, sizeof g_places[0].name, "%s", col[want]);
        g_nplaces++;
    }
    fclose(f);
    LOG("presence: %d place names in %s (%s, for language %s)", g_nplaces, path, took[0] ? took : "no column", lang);
}
// the game's name for a place ID, else the ID's words (KokiriForest: "Kokiri Forest")
static void place_name(const char *id, char *out, size_t n) {
    for (int i = 0; i < g_nplaces; i++) if (!strcmp(g_places[i].id, id)) { snprintf(out, n, "%s", g_places[i].name); return; }
    size_t k = 0;
    for (const char *c = id; *c && k + 2 < n; c++) {
        int up = *c >= 'A' && *c <= 'Z', after = c > id && ((c[-1] >= 'a' && c[-1] <= 'z') || (c[-1] >= '0' && c[-1] <= '9'));
        if (*c == '_') { out[k++] = ' '; continue; }
        if (up && after) out[k++] = ' ';
        out[k++] = *c;
    }
    out[k] = 0;
}
// a map's place: its ID without the age (Market_Child: Market)
static void map_place(const char *map, char *out, size_t n) {
    char id[65];
    snprintf(id, sizeof id, "%s", map);
    char *age = strstr(id, "_Child");
    if (!age) age = strstr(id, "_Adult");
    if (age) *age = 0;
    place_name(id, out, n);
}

// ---- the activity ----
typedef struct { char details[129], state[129]; int64_t start; } activity;   // start: Unix seconds, from which Discord counts the time shown

static void cat(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cat(char *b, size_t n, const char *fmt, ...) {
    size_t k = strlen(b);
    if (k + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + k, n - k, fmt, ap);
    va_end(ap);
}
static void hearts(char *out, size_t n, int quarters) {
    static const char *frac[] = {"", "¼", "½", "¾"};
    if (quarters < 4 && quarters % 4) snprintf(out, n, "%s", frac[quarters % 4]);
    else snprintf(out, n, "%d%s", quarters / 4, frac[quarters % 4]);
}
static void activity_of(const dm_gd_state *s, int ok, int64_t now, int64_t session, activity *a) {
    memset(a, 0, sizeof *a);
    a->start = session;
    if (!ok || s->menu == DM_GD_MENU_TITLE || (!s->place[0] && !s->map[0])) { snprintf(a->details, sizeof a->details, "Title screen"); return; }
    if (s->place[0]) place_name(s->place, a->details, sizeof a->details);
    else map_place(s->map, a->details, sizeof a->details);
    if (strlen(a->details) < 2) snprintf(a->details, sizeof a->details, "Hyrule");   // Discord refuses a field shorter than 2
    if (s->menu == DM_GD_MENU_GAME_OVER) snprintf(a->state, sizeof a->state, "Game Over");
    else {
        if (s->child >= 0) cat(a->state, sizeof a->state, "%s", s->child ? "Child" : "Adult");
        if (s->life >= 0 && s->max_life > 0) {
            char h[16], m[16];
            hearts(h, sizeof h, s->life < s->max_life ? s->life : s->max_life);
            hearts(m, sizeof m, s->max_life);
            cat(a->state, sizeof a->state, "%s♥ %s/%s", a->state[0] ? SEP : "", h, m);
        }
        if (s->rupees >= 0) cat(a->state, sizeof a->state, "%s%d rupee%s", a->state[0] ? SEP : "", s->rupees, s->rupees == 1 ? "" : "s");
        if (s->paused == 1) cat(a->state, sizeof a->state, "%sPaused", a->state[0] ? SEP : "");
    }
    if (s->play_time >= 0 && s->play_time <= now) a->start = now - s->play_time;   // Discord then shows the file's play time, counting on
}
static void json_str(char *b, size_t n, const char *s) {
    cat(b, n, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') cat(b, n, "\\%c", c);
        else if (c < 0x20) cat(b, n, "\\u%04x", c);
        else cat(b, n, "%c", c);
    }
    cat(b, n, "\"");
}
// 0 when it would not fit
static int activity_json(const activity *a, int pid, unsigned nonce, char *b, size_t n) {
    b[0] = 0;
    cat(b, n, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"name\":", pid);
    json_str(b, n, DM_GAME_NAME);   // Discord shows it in place of the application's name
    cat(b, n, ",\"type\":0,\"details\":");
    json_str(b, n, a->details);
    if (a->state[0]) { cat(b, n, ",\"state\":"); json_str(b, n, a->state); }
    cat(b, n, ",\"timestamps\":{\"start\":%lld}", (long long)a->start * 1000);
    if (*DM_GAME_DISCORD_IMAGE) { cat(b, n, ",\"assets\":{\"large_image\":"); json_str(b, n, DM_GAME_DISCORD_IMAGE); cat(b, n, ",\"large_text\":"); json_str(b, n, HOVER_TEXT); cat(b, n, "}"); }
    cat(b, n, "}},\"nonce\":\"%u\"}", nonce);
    return strlen(b) + 1 < n;
}

// ---- Discord's socket ----
static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *c = p;
    while (n) {
        ssize_t r = write(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        c += r; n -= (size_t)r;
    }
    return 1;
}
static int read_all(int fd, void *p, size_t n) {
    uint8_t *c = p;
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        c += r; n -= (size_t)r;
    }
    return 1;
}
static int frame_write(int fd, uint32_t op, const char *s, uint32_t n) {
    uint32_t h[2] = {op, n};   // little-endian, as this Mac
    return write_all(fd, h, sizeof h) && write_all(fd, s, n);
}
// a frame's payload, NUL-terminated and cut to cap: its length; -2 when none came within timeout_ms, -1 when the socket failed or closed
static int frame_read(int fd, uint32_t *op, char *buf, size_t cap, int timeout_ms) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return -2;
    if (r < 0) return errno == EINTR ? -2 : -1;
    uint32_t h[2];
    if (!read_all(fd, h, sizeof h) || h[1] > (1u << 20)) return -1;
    size_t keep = h[1] < cap - 1 ? h[1] : cap - 1;
    if (!read_all(fd, buf, keep)) return -1;
    for (size_t left = h[1] - keep; left;) {
        char sink[512];
        size_t k = left < sizeof sink ? left : sizeof sink;
        if (!read_all(fd, sink, k)) return -1;
        left -= k;
    }
    buf[keep] = 0;
    *op = h[0];
    return (int)keep;
}
// $TMPDIR, then the user's own temporary folder when $TMPDIR is another (a start from a script): Discord listens in the one launchd gave it
static int sock_dirs(char dirs[2][900]) {
    int n = 0;
    const char *t = getenv("TMPDIR");
    if (t && *t) snprintf(dirs[n++], sizeof dirs[0], "%s", t);
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, dirs[n], sizeof dirs[0])) n++;
    for (int i = 0; i < n; i++)
        for (size_t l = strlen(dirs[i]); l > 1 && dirs[i][l - 1] == '/'; l--) dirs[i][l - 1] = 0;
    if (n == 2 && !strcmp(dirs[0], dirs[1])) n = 1;
    return n;
}
// the handshake's answer is READY, or a close that says why (an unknown application ID: *refused)
static int ipc_connect(const char *app, const char *dir, int *refused) {
    for (int i = 0; i < 10; i++) {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        if (snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-%d", dir, i) >= (int)sizeof a.sun_path) return -1;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        struct timeval tv = {2, 0};   // a Discord that stops answering mid-frame does not hold the thread
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); continue; }
        char hs[96], buf[2048];
        uint32_t op = 0;
        int n = snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", app);
        if (!frame_write(fd, OP_HANDSHAKE, hs, (uint32_t)n) || (n = frame_read(fd, &op, buf, sizeof buf, 5000)) < 0) { close(fd); continue; }
        if (op == OP_FRAME && strstr(buf, "\"READY\"")) { LOG("presence: connected to Discord (%s)", a.sun_path); return fd; }
        LOG("[RED] presence: Discord (%s) refused application %s: %.300s", a.sun_path, app, buf);
        *refused = 1;
        close(fd);
        return -1;
    }
    return -1;
}

static char g_app[32];
static int64_t g_session;

static void *presence_main(void *arg) {
    (void)arg;
    pthread_setname_np("dm presence");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    char dirs[2][900], json[2048], buf[16384], logged[129] = "";
    int ndirs = sock_dirs(dirs);
    if (!ndirs) { LOG("[RED] presence: no temporary folder to look for Discord's socket in"); return NULL; }
    int fd = -1, have_sent = 0, said_absent = 0, said_why = 0, reds = 0;
    int64_t next_connect = 0, last_send = 0, last_read = -READ_EVERY_S;
    unsigned nonce = 0;
    activity cur, sent;
    memset(&cur, 0, sizeof cur);
    memset(&sent, 0, sizeof sent);
    for (;;) {
        int64_t now = time(NULL);
        if (fd < 0 && now >= next_connect) {
            int refused = 0;
            fd = -1;
            for (int i = 0; fd < 0 && !refused && i < ndirs; i++) fd = ipc_connect(g_app, dirs[i], &refused);
            have_sent = 0;
            if (fd < 0) {
                next_connect = now + (refused ? REFUSED_RETRY_S : RETRY_S);
                if (!refused && !said_absent++) LOG("presence: Discord is not running (no discord-ipc socket in %s%s%s): looking for it every %d s", dirs[0], ndirs > 1 ? " or " : "", ndirs > 1 ? dirs[1] : "", RETRY_S);
            }
        }
        if (now - last_read >= READ_EVERY_S) {
            dm_gd_state s;
            const char *why = "";
            int ok = dm_gd_read(&s, &why);
            if (!ok && !said_why && strstr(why, "profile")) { said_why = 1; LOG("presence: %s (%s): the activity shows no place", why, dm_main_build()); }
            activity_of(&s, ok, now, g_session, &cur);
            last_read = now;
        }
        if (fd >= 0) {
            uint32_t op = 0;
            int n;
            while ((n = frame_read(fd, &op, buf, sizeof buf, 0)) >= 0) {
                if (op == OP_PING) { if (!frame_write(fd, OP_PONG, buf, (uint32_t)n)) { n = -1; break; } }
                else if (op == OP_CLOSE) { n = -1; break; }
                else if (op == OP_FRAME && strstr(buf, "\"evt\":\"ERROR\"") && reds++ < 4) LOG("[RED] presence: Discord refused the activity: %.300s", buf);
            }
            int changed = !have_sent || strcmp(cur.details, sent.details) || strcmp(cur.state, sent.state) || llabs(cur.start - sent.start) > 60;
            if (n != -1 && changed && now - last_send >= SEND_EVERY_S && activity_json(&cur, getpid(), ++nonce, json, sizeof json)) {
                if (frame_write(fd, OP_FRAME, json, (uint32_t)strlen(json))) {
                    sent = cur; have_sent = 1; last_send = now;
                    if (strcmp(cur.details, logged)) { LOG("presence: %s%s%s", cur.details, cur.state[0] ? SEP : "", cur.state); snprintf(logged, sizeof logged, "%s", cur.details); }
                } else n = -1;
            }
            if (n == -1) {
                close(fd);
                fd = -1;
                next_connect = now + RETRY_S;
                LOG("presence: the connection to Discord closed: looking for it again in %d s", RETRY_S);
                continue;
            }
            struct pollfd p = {fd, POLLIN, 0};
            poll(&p, 1, 1000);
        } else sleep(1);
    }
    return NULL;
}

void dm_presence_start(const char *dir) {
    const char *on = getenv("DM_DISCORD");
    if (on ? strcmp(on, "1") : !getenv("DM_WINDOW")) return;   // unset: on with a window only, so headless runs show nothing
    const char *app = getenv("DM_DISCORD_APP");
    if (!app || !*app) app = DM_GAME_DISCORD_APP;
    size_t n = strlen(app);
    if (n < 15 || n >= sizeof g_app || strspn(app, "0123456789") != n) {
        LOG("presence: no Discord application ID (the profile's discord_app or DM_DISCORD_APP): no Discord presence");
        return;
    }
    memcpy(g_app, app, n + 1);
    const char *lang = getenv("DM_LANG");
    places_load(dir, lang && *lang ? lang : "en-US");
    g_session = time(NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, presence_main, NULL)) { LOG("[RED] presence: its thread did not start: no Discord presence"); return; }
    pthread_detach(th);
}
