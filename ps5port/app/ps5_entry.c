// DevilutionX for PS5 - process entry.
//
// The game's own main() is untouched: the link wraps it (--wrap=main) so this
// runs first. It leaves the sandbox, picks the output mode, points the game's
// asset, save and settings lookups at the console's folders, then hands control to the game.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps5_unlock.h"

extern int __real_main(int argc, char** argv, char** envp);
extern void SDL_SetMainReady(void) __attribute__((weak));
// The system settings' values (libSceSystemService); id 1 is the language.
extern int sceSystemServiceParamGetInt(int id, int* value);
// What the game and the Vulkan driver write to stderr goes to the console's klog (nc <PS5 IP> 3232).
extern int ps5_klog_capture_stderr(const char* prefix) __attribute__((weak));

// The mode the TV is asked for (sdl2/ps5vk.c picks the closest at about 60 Hz). Weak, so the configure-time
// link checks of the build work without the graphics libraries.
extern void ps5vk_prefer_mode(int width, int height) __attribute__((weak));

#define RENDER_WIDTH 3840
#define RENDER_HEIGHT 2160

// Where the files the user brings go (diabdat.mpq, spawn.mpq, hellfire.mpq, hf*.mpq, mods/, fonts.mpq)
// and where the game writes its saves and diablo.ini.
#define DATA_ROOT "/data/DevilutionX"
#define TITLE_ID "PPSA98514"

// How much the app-owned heap reserves (app_heap.c); the game, its decoded
// sounds and Mesa's shader compiler share it.
const size_t ps5_opengl_heap_size = 768u * 1024u * 1024u;

// The console's getcwd faults, and std::filesystem::current_path() needs it:
// the link wraps it (--wrap=getcwd) with the folder the app runs from.
static char g_cwd[256] = "/";

char* __wrap_getcwd(char* buffer, size_t size) {
    size_t needed = strlen(g_cwd) + 1;
    if (!buffer) {
        buffer = malloc(size > needed ? size : needed);
        if (!buffer)
            return NULL;
        size = size > needed ? size : needed;
    }
    if (size < needed) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, g_cwd, needed);
    return buffer;
}

// The folder holding devilutionx.mpq: /app0 inside the sandbox, otherwise where the
// loader installed the title.
static const char* app_root(void) {
    static const char* const roots[] = {
        "/app0", "/system_ex/app/" TITLE_ID, "/data/homebrew/" TITLE_ID,
        "/mnt/ext0/homebrew/" TITLE_ID, "/mnt/ext1/homebrew/" TITLE_ID,
        "/mnt/usb0/homebrew/" TITLE_ID, "/mnt/usb1/homebrew/" TITLE_ID,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char probe[256];
        struct stat info;
        snprintf(probe, sizeof(probe), "%s/devilutionx.mpq", roots[i]);
        if (stat(probe, &info) == 0 && S_ISREG(info.st_mode))
            return roots[i];
    }
    return NULL;
}

// Leaving the sandbox: the same sequence as the project this port follows. The
// HEN's jailbreak-on-demand goes first, then the Lapy helper through the ELF
// loader; the route that worked last time is remembered and tried first.
extern int DevilutionX_elevate(const char** route);

static const char* const NO_UNLOCK_MARKERS[] = {"/download0/DevilutionX_no_unlock", DATA_ROOT "/no_unlock"};

static int path_exists(const char* path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static int unlock_disabled(void) {
    for (size_t i = 0; i < 2; ++i)
        if (path_exists(NO_UNLOCK_MARKERS[i]))
            return 1;
    return 0;
}

/* How the storage was unlocked, remembered between starts: the way that worked last time goes first. */
enum { UNLOCK_ROUTE_NONE, UNLOCK_ROUTE_HEN, UNLOCK_ROUTE_LAPY };
#define UNLOCK_ROUTE_FILE DATA_ROOT "/cache/unlock-route"

static int unlock_route_remembered(void) {
    char word[16] = "";
    FILE* f = fopen(UNLOCK_ROUTE_FILE, "r");
    if (!f)
        return UNLOCK_ROUTE_NONE;
    if (!fgets(word, sizeof(word), f))
        word[0] = 0;
    fclose(f);
    return strncmp(word, "lapy", 4) == 0 ? UNLOCK_ROUTE_LAPY : strncmp(word, "hen", 3) == 0 ? UNLOCK_ROUTE_HEN : UNLOCK_ROUTE_NONE;
}

static void unlock_route_remember(int route) {
    if (route == UNLOCK_ROUTE_NONE) {
        remove(UNLOCK_ROUTE_FILE);
        return;
    }
    if (unlock_route_remembered() == route)
        return;
    mkdir(DATA_ROOT, 0777);
    mkdir(DATA_ROOT "/cache", 0777); /* the folder may not exist yet at the very first start */
    FILE* f = fopen(UNLOCK_ROUTE_FILE, "w");
    if (!f)
        return;
    fputs(route == UNLOCK_ROUTE_LAPY ? "lapy\n" : "hen\n", f);
    fclose(f);
}

static int prepare_storage(void) {
    if (unlock_disabled())
        return ps5_data_listable();
    /* The way that worked at the last start goes first, so a console that always answers the same way does not wait
     * for the others to stay silent (each takes a second or more). If it fails, the usual order runs. */
    const int lapy_first = !ps5_data_listable() && unlock_route_remembered() == UNLOCK_ROUTE_LAPY;
    const char* route = "none";
    int status = -1;
    int lapy_tried = 0;
    if (lapy_first) {
        /* a loaded LegacyJB / etaHEN answers a short request at once, and is faster than the helper */
        if (ps5_unlock_quick() == UNLOCK_OK) {
            unlock_route_remember(UNLOCK_ROUTE_HEN);
            return 1;
        }
        status = DevilutionX_elevate(&route);
        lapy_tried = 1;
        if (status == 0 && ps5_data_listable())
            return 1;
    }
    /* 1. etaHEN's jailbreak-on-demand (answered on 13.60). */
    UnlockResult hen = ps5_unlock_etahen();
    if (hen == UNLOCK_ALREADY)
        return 1;
    if (hen == UNLOCK_OK) {
        unlock_route_remember(UNLOCK_ROUTE_HEN);
        return 1;
    }
    /* 2. The boilerplate's Lapy helper through the ELF loader (6.02/12.70). A request is made once per process, so
     * not again when it already went first. */
    if (!lapy_tried) {
        status = DevilutionX_elevate(&route);
        if (status == 0 && ps5_data_listable()) {
            unlock_route_remember(UNLOCK_ROUTE_LAPY);
            return 1;
        }
    }
    unlock_route_remember(UNLOCK_ROUTE_NONE); /* nothing answered: forget the old way */
    /* 3. Sandboxed: files in /data still open and save, folders can't be listed. */
    return 0;
}

// The system closes the title: exit() and _exit() raise SIGSYS on the console. LoadExec
// "exit" ends the title normally (the game's quit() and main() both end in exit()).
extern int sceSystemServiceLoadExec(const char* path, const char** arguments);

static void close_title(void) {
    fflush(NULL);
    for (;;) {
        sceSystemServiceLoadExec("exit", NULL);
        sleep(1); /* until the shell takes the title down; asked again if it did not */
    }
}

void __wrap_exit(int status) {
    (void)status;
    close_title();
}

// Called by the startup code when main() returns, before it would call exit().
void catchReturnFromMain(int status) {
    (void)status;
    close_title();
}

// First thing, single-threaded: the HEN does not grant a process that already runs threads.
__attribute__((constructor(101))) static void early_init(void) {
    prepare_storage();
}

// The console's language as the locales DevilutionX tries, best first. It reads them from $LANGUAGE when the
// game's language is not in diablo.ini yet (Source/platform/locale.cpp), so the first start follows the console and
// a language chosen in the game's settings wins afterwards. Spanish from Spain and from Latin America are both "es".
static const char* console_locales(int lang) {
    switch (lang) {
    case 0: return "ja";
    case 2: case 22: return "fr";          // French, Canadian French
    case 3: case 20: return "es";          // Spanish, Latin American Spanish
    case 4: return "de";
    case 5: return "it";
    case 7: return "pt_PT:pt_BR";          // Portuguese: the one Portuguese translation is Brazilian
    case 8: return "ru";
    case 9: return "ko";
    case 10: return "zh_TW";
    case 11: return "zh_CN";
    case 12: return "fi";
    case 13: return "sv";
    case 14: return "da";
    case 16: return "pl";
    case 17: return "pt_BR";
    case 19: return "tr";
    case 23: return "cs";
    case 24: return "hu";
    case 25: return "el";
    case 26: return "ro";
    default: return "en";                  // English, and the languages the game has no translation for
    }
}

int __wrap_main(int argc, char** argv, char** envp) {
    if (ps5_klog_capture_stderr)
        ps5_klog_capture_stderr("[DevilutionX] ");
    const char* root = app_root();
    if (root) {
        // the sandbox refuses chdir: the game finds its files from SDL_GetBasePath()
        // (patched to read this variable) and from the wrapped getcwd()
        char base[256];
        snprintf(base, sizeof(base), "%s/", root);
        setenv("SDL_PS5_BASE_PATH", base, 1);
        snprintf(g_cwd, sizeof(g_cwd), "%s", root);
    }

    // The user's files live in DATA_ROOT. The game looks for its MPQs in the app folder (devilutionx.mpq,
    // mods/hf.mpq: SDL_GetBasePath) and in the save folder, which is DATA_ROOT; diablo.ini goes there too.
    mkdir(DATA_ROOT, 0777);
    mkdir(DATA_ROOT "/mods", 0777);
    setenv("XDG_DATA_HOME", DATA_ROOT, 1);
    setenv("HOME", DATA_ROOT, 1);

    int console_language = 1;
    if (sceSystemServiceParamGetInt(1, &console_language) == 0) {
        const char* locales = console_locales(console_language);
        setenv("LANGUAGE", locales, 0); // unless the caller already set one
#ifdef PS5_DEBUG_LOG
        fprintf(stderr, "console language %d -> %s\n", console_language, locales);
#endif
    }

    // The console only has a full-screen output: render at 4K; the console scales it to the TV.
    if (ps5vk_prefer_mode)
        ps5vk_prefer_mode(RENDER_WIDTH, RENDER_HEIGHT);

    if (SDL_SetMainReady)
        SDL_SetMainReady();

    // --save-dir / --config-dir unless the caller gave them (--data-dir stays free: it would replace the app folder)
    int has_save = 0, has_config = 0;
    for (int i = 1; i < argc; i++) {
        has_save |= strcmp(argv[i], "--save-dir") == 0;
        has_config |= strcmp(argv[i], "--config-dir") == 0;
    }
    char** args = (char**)malloc(sizeof(char*) * (size_t)(argc + 5));
    int count = 0;
    for (int i = 0; i < argc; i++)
        args[count++] = argv[i];
    if (!has_save) {
        args[count++] = (char*)"--save-dir";
        args[count++] = (char*)DATA_ROOT "/";
    }
    if (!has_config) {
        args[count++] = (char*)"--config-dir";
        args[count++] = (char*)DATA_ROOT "/";
    }
    args[count] = NULL;
    return __real_main(count, args, envp);
}
