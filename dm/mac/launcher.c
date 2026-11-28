// Mac app launcher: sets the host's environment from the bundle, shows the settings window, then runs Contents/MacOS/dmhost as a child. Not exec'ed:
// TCC attributes a child's file access to the app, while an exec'ed process gets EPERM on every open on a removable volume, with no prompt.
// Sources of the environment, in order: docs/frontends.md#saved-choices.
#include <CoreFoundation/CoreFoundation.h>
#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <removefile.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "romfs_image.h"

extern char **environ;
void dm_mac_record_launch_keys(void);   // Launcher.swift: DM_ENV_LAUNCH_KEYS
int dm_mac_launcher(int show_window);   // Launcher.swift: 1 = start the game, the settings in the environment; 0 = quit
int dm_mac_microphone_consent(void);
static pid_t g_child;
static char *g_file_keys;

static void add_key(char **list, const char *k) {
    char *s;
    if (asprintf(&s, "%s%s%s", *list ? *list : "", *list ? " " : "", k) < 0) { fputs("[RED] launcher: out of memory\n", stderr); exit(1); }
    free(*list); *list = s;
}

static void forward(int sig) { if (g_child > 0) kill(g_child, sig); }

static int env_is(const char *key, const char *value) { const char *s = getenv(key); return s && !strcmp(s, value); }

static int info_string(const char *key, char *out, size_t n) {
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k); CFRelease(k);
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8) && *out;
}

static void alert(const char *title, const char *msg) {
    fprintf(stderr, "[RED] %s\n", msg);
    char *script = "on run argv\ndisplay alert (item 1 of argv) message (item 2 of argv) as critical\nend run";
    char *argv[] = {"osascript", "-e", script, (char *)title, (char *)msg, NULL};
    pid_t pid;
    int st;
    if (!posix_spawnp(&pid, "osascript", NULL, NULL, argv, environ)) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

static int modules_present(const char *dm) {
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFTypeRef files = CFBundleGetValueForInfoDictionaryKey(b, CFSTR("DMModuleFiles"));
    if (!files || CFGetTypeID(files) != CFArrayGetTypeID() || !CFArrayGetCount(files)) return 0;
    for (CFIndex i = 0; i < CFArrayGetCount(files); i++) {
        CFTypeRef v = CFArrayGetValueAtIndex(files, i);
        char name[PATH_MAX], path[PATH_MAX];
        struct stat st;
        if (CFGetTypeID(v) != CFStringGetTypeID() || !CFStringGetCString(v, name, sizeof name, kCFStringEncodingUTF8) || !*name || strchr(name, '/')) return 0;
        int n = snprintf(path, sizeof path, "%s/%s", dm, name);
        if (n < 0 || (size_t)n >= sizeof path || stat(path, &st) || !S_ISREG(st.st_mode) || access(path, R_OK)) return 0;
    }
    return 1;
}

static void mkdirs(const char *path) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

// nxnative's builds keep the folder and choices under its names: a first start takes an APFS clone of the old folder and the old domain's missing keys
static int take_folder(const char *old, const char *now) {
    struct stat st; char part[PATH_MAX];
    if (!lstat(now, &st) || stat(old, &st) || !S_ISDIR(st.st_mode)) return 0;
    snprintf(part, sizeof part, "%s.partial", now);
    removefile(part, NULL, REMOVEFILE_RECURSIVE);
    if (copyfile(old, part, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE) || rename(part, now)) {
        fprintf(stderr, "[RED] launcher: cannot copy %s to %s: %s; this app starts without it\n", old, now, strerror(errno));
        removefile(part, NULL, REMOVEFILE_RECURSIVE); return 0;
    }
    fprintf(stderr, "launcher: %s taken from %s (a copy: the old folder stays)\n", now, old);
    return 1;
}

#define BUNDLE_PREFIX "org.nativenavi."

static void take_choices(void) {
    CFStringRef now = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!now || !CFStringHasPrefix(now, CFSTR(BUNDLE_PREFIX))) return;   // packed with another bundle ID: no old domain to name
    CFIndex skip = sizeof BUNDLE_PREFIX - 1;
    CFStringRef rest = CFStringCreateWithSubstring(NULL, now, CFRangeMake(skip, CFStringGetLength(now) - skip));
    CFStringRef old = CFStringCreateWithFormat(NULL, NULL, CFSTR("org.nxnative.%@"), rest); CFRelease(rest);
    CFArrayRef keys = CFPreferencesCopyKeyList(old, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFIndex taken = 0;
    for (CFIndex i = 0; keys && i < CFArrayGetCount(keys); i++) {
        CFStringRef key = CFArrayGetValueAtIndex(keys, i);
        CFPropertyListRef have = CFPreferencesCopyAppValue(key, now);
        if (have) { CFRelease(have); continue; }
        CFPropertyListRef v = CFPreferencesCopyAppValue(key, old);
        if (v) { CFPreferencesSetAppValue(key, v, now); CFRelease(v); taken++; }
    }
    char name[256] = "";
    CFStringGetCString(old, name, sizeof name, kCFStringEncodingUTF8);
    if (taken && CFPreferencesAppSynchronize(now)) fprintf(stderr, "launcher: %ld saved choices taken from %s\n", (long)taken, name);
    else if (taken) fprintf(stderr, "[RED] launcher: the saved choices of %s could not be written to this app's preferences\n", name);
    if (keys) CFRelease(keys);
    CFRelease(old);
}

static void take_nxnative(const char *home, const char *id, const char *sup) {
    char base[PATH_MAX], old[PATH_MAX], now[PATH_MAX];
    snprintf(base, sizeof base, "%s/Library/Application Support/NativeNavi", home); mkdirs(base);
    snprintf(old, sizeof old, "%s/Library/Application Support/nxnative/%s", home, id);
    if (take_folder(old, sup)) take_choices();
    snprintf(old, sizeof old, "%s/Library/Application Support/nxnative/system", home); snprintf(now, sizeof now, "%s/system", base);
    take_folder(old, now);
}

static void env_file(const char *path) {   // a variable already set stays
    FILE *f = fopen(path, "r"); if (!f) return;
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        char *s = ln; while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq || eq == s) continue;
        *eq = 0;
        char *k = s;
        for (char *t = eq - 1; t > k && (*t == ' ' || *t == '\t'); t--) *t = 0;
        if (!getenv(k)) {
            setenv(k, eq + 1, 0);
            add_key(&g_file_keys, k);
            fprintf(stderr, "launcher: env %s=%s (%s)\n", k, eq + 1, path);
        }
    }
    fclose(f);
}

// a denial turns capture off for this run only
static void microphone_at_launch(void) {
    if (!env_is("DM_MIC", "1")) return;
    if (!CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("NSMicrophoneUsageDescription")) ||
        !dm_mac_microphone_consent()) {
        setenv("DM_MIC", "0", 1);
        return;
    }
    CFStringRef domain = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (getenv("DM_MIC_DEVICE") || env_is("DM_SAVED_CHOICES", "0") || !domain) return;
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("launcher.DM_MIC_DEVICE"), domain);
    char uid[256];
    if (value && CFGetTypeID(value) == CFStringGetTypeID() && CFStringGetCString(value, uid, sizeof uid, kCFStringEncodingUTF8) && *uid)
        setenv("DM_MIC_DEVICE", uid, 0);
    if (value) CFRelease(value);
}

int main(int argc, char **argv) {
    char self[PATH_MAX], exe[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) || !realpath(self, exe)) return 1;
    char contents[PATH_MAX], res[PATH_MAX], host[PATH_MAX], dm[PATH_MAX], romfs[PATH_MAX], p[PATH_MAX];
    snprintf(contents, sizeof contents, "%s", exe); *strrchr(contents, '/') = 0; *strrchr(contents, '/') = 0;
    snprintf(res, sizeof res, "%s/Resources", contents); snprintf(host, sizeof host, "%s/MacOS/dmhost", contents);
    snprintf(dm, sizeof dm, "%s/dm", res); snprintf(romfs, sizeof romfs, "%s/romfs", res);
    char name[256] = "NativeNavi", id[64] = "", marker[512] = "", lang[32] = "";
    if (!info_string("CFBundleName", name, sizeof name)) snprintf(name, sizeof name, "NativeNavi");
    info_string("DMRomfsMarker", marker, sizeof marker); info_string("DMLanguage", lang, sizeof lang);
    if (!info_string("DMTitleID", id, sizeof id) || strchr(id, '/')) {
        alert(name, "This app has no title ID in its Info.plist (DMTitleID): pack it again with build_app.py --platform mac-app.");
        return 1;
    }
    struct stat st; snprintf(p, sizeof p, "%s/romfs.img", res);
    int valid_romfs;
    if (!stat(p, &st)) {
        romfs_img *image = romfs_image_open(p); valid_romfs = image != NULL;
        romfs_image_close(image); snprintf(romfs, sizeof romfs, "%s", p);
    } else { snprintf(p, sizeof p, "%s/%s", romfs, marker); valid_romfs = !stat(p, &st); }
    if (access(host, X_OK) || !valid_romfs || !modules_present(dm)) {
        alert(name, "The game data inside this app is missing or incomplete (Contents/Resources/romfs.img, Contents/Resources/dm or Contents/MacOS/dmhost): pack it again with build_app.py --platform mac-app.");
        return 1;
    }
    const char *home = getenv("HOME"); if (!home || !*home) return 1;
    char sup[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], logs[PATH_MAX], log[PATH_MAX], prev[PATH_MAX];
    snprintf(sup, sizeof sup, "%s/Library/Application Support/NativeNavi/%s", home, id);
    snprintf(save, sizeof save, "%s/save", sup); snprintf(temp, sizeof temp, "%s/temp", sup);
    snprintf(logs, sizeof logs, "%s/Library/Logs/NativeNavi", home);
    snprintf(log, sizeof log, "%s/%s.log", logs, id); snprintf(prev, sizeof prev, "%s/%s.prev.log", logs, id);
    mkdirs(logs);
    rename(log, prev);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    fprintf(stderr, "launcher: %s (%s), host %s\n", name, id, host);
    take_nxnative(home, id, sup);   // before anything makes the folder
    mkdirs(save); mkdirs(temp);
    dm_mac_record_launch_keys();
    snprintf(p, sizeof p, "%s/env.txt", sup); env_file(p);
    snprintf(p, sizeof p, "%s/env.txt", dm); env_file(p);
    setenv("DM_ENV_FILE_KEYS", g_file_keys ? g_file_keys : "", 1);
    free(g_file_keys);
    setenv("DM_SAVE", save, 0);   // before the window, which the host is started from
    int window = !env_is("DM_LAUNCHER", "0");
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-launcher")) window = 0;
    if (!window) fprintf(stderr, "launcher: no settings window (DM_LAUNCHER=0 or --no-launcher)\n");
    if (!dm_mac_launcher(window)) return 0;
    microphone_at_launch();
    setenv("DM_ROOT", sup, 0); setenv("DM_ROMFS", romfs, 0); setenv("DM_TEMP", temp, 0);
    snprintf(p, sizeof p, "%s/Library/Application Support/NativeNavi/system", home); setenv("DM_SYSTEM_DIR", p, 0);
    snprintf(p, sizeof p, "%s/msl", res); setenv("DM_MSL_DIR", p, 0);
    snprintf(p, sizeof p, "%s/font", dm); setenv("DM_FONT_DIR", p, 0);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dm); if (!stat(p, &st)) setenv("DM_NVN_SHMAP", p, 0);
    snprintf(p, sizeof p, "%s/aoc", res); if (!stat(p, &st)) setenv("DM_AOC_DIR", p, 0);
    snprintf(p, sizeof p, "%s/pso", res); if (!stat(p, &st)) setenv("DM_PSO_BUNDLE", p, 0);
    if (*lang) setenv("DM_LANG", lang, 0);
    setenv("DM_WINDOW", "1", 0); setenv("DM_AUDIO", "1", 0);
    if (chdir(sup)) { fprintf(stderr, "[RED] launcher: cannot enter %s\n", sup); return 1; }
    struct sigaction sa = {.sa_handler = forward};   // no SA_RESTART: waitpid below sees EINTR and waits again
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    char *host_argv[] = {host, dm, NULL};
    int err = posix_spawn(&g_child, host, NULL, NULL, host_argv, environ);
    if (err) { fprintf(stderr, "[RED] launcher: cannot start %s: %s\n", host, strerror(err)); return 1; }
    int status;
    while (waitpid(g_child, &status, 0) < 0) if (errno != EINTR) return 1;
    if (WIFSIGNALED(status)) { fprintf(stderr, "launcher: the host stopped on signal %d\n", WTERMSIG(status)); return 128 + WTERMSIG(status); }
    fprintf(stderr, "launcher: the host exited with %d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
