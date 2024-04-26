#include "nc1020.h"

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <muteki/file.h>
#include <muteki/ini.h>
#include <muteki/memory.h>
#include <muteki/system.h>
#include <muteki/threading.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>

const key_press_event_config_t KEY_EVENT_CONFIG_DRAIN = {65535, 65535, 1};
const key_press_event_config_t KEY_EVENT_CONFIG_TURBO = {0, 0, 0};

const char ROM_FILE[] = "rom.bin";
const char NOR_FILE[] = "nor.bin";
const char BBS_FILE[] = "bbs.bin";
const char STATE_FILE[] = "nc1020.sts";
const char CONFIG_FILE[] = "nc1020.ini";

// Reserve 1MB on the heap so we don't get an OOM in case OS is allocating short-lived objects on heap.
// 1MiB seems reasonable but this may needs to be adjusted further if it's proven to not be enough.
constexpr size_t HEAP_RESERVED = 1024 * 1024;

const uint8_t KEYMAP_0x01[7] = {0x3b, 0x3f, 0x1a, 0x1f, 0x1b, 0x37, 0x1e}; // KEY_ESC - KEY_PGDN
const uint8_t KEYMAP_ALPHABETS[26] = {
    0x28, 0x34, 0x32, 0x2a, 0x22, 0x2b, 0x2c, 0x2d, 0x27, 0x2e, 0x2f, 0x19, 0x36,
    0x35, 0x18, 0x1c, 0x20, 0x23, 0x29, 0x24, 0x26, 0x33, 0x21, 0x31, 0x25, 0x30,
}; // KEY_A - KEY_Z
const uint8_t KEYMAP_NUMBERS[10] = {
    0x08, 0x10, 0x11, 0x12, 0x13, 0x0b, 0x0c, 0x0d, 0x0a, 0x09,
    // time, F1, F2, F3, F4, dict, vcard, calc, calendar, exam
}; // KEY_0 - KEY_9

static const uint8_t FLAG_ROM_VOLUME_0 = 0b000;
static const uint8_t FLAG_ROM_VOLUME_1 = 0b001;
static const uint8_t FLAG_ROM_VOLUME_2 = 0b010;
static const uint8_t FLAG_NOR = 0b011;
static const uint8_t FLAG_NOR_DIRTY = 0b100;

struct CacheBlock {
    uint8_t flags; // xxxxxdVV. d: NOR page dirty, V: Volume number, 3 means NOR.
    uint8_t page;
    uint8_t data[0x8000];
};

// 1 cache block and a pointer.
constexpr size_t CACHE_OVERHEAD_UNIT = sizeof(CacheBlock) + 4;
// 3 ROM volumes and NOR pages
constexpr size_t MAX_CACHE_SIZE = 0x80 * 3 + 0x20;

class WqxHalBesta : public wqx::IWqxHal {
public:
    WqxHalBesta();
    virtual bool loadNorPage(uint32_t page) override;
    virtual bool saveNorPage(uint32_t page) override;
    virtual bool wipeNorFlash() override;
    virtual bool loadRomPage(uint32_t volume, uint32_t page) override;
    virtual bool loadBbsPage(uint32_t volume, uint32_t page) override;
    virtual bool saveState(const char *states, size_t size) override;
    virtual bool loadState(char *states, size_t size) override;
    void closeAll();
    bool ensureOpen();
    bool begin(size_t cacheSize);
private:
    unsigned short getNorCacheIndex(uint32_t page);
    void setNorCacheIndex(uint32_t page, unsigned short index);
    unsigned short getRomCacheIndex(uint32_t volume, uint32_t page);
    void setRomCacheIndex(uint32_t volume, uint32_t page, unsigned short index);
    CacheBlock *claimPage(unsigned short cacheIndex);

    void *romFile;
    void *norFile;
    void *bbsFile;
    size_t firstOut;
    size_t cacheSize;
    int8_t currentMappedNorPage;
    CacheBlock **cacheBlockTable;
    CacheBlock *cacheBlock;
    uint8_t romIndexLow[0x80 * 3];
    uint32_t romIndexHigh[0x80 * 3 / 32];
    uint8_t norIndexLow[0x20];
    uint32_t norIndexHigh;
    uint8_t bbsCache[0x20000];

    static constexpr unsigned short CACHE_INDEX_UNUSED = 0x1ff;
};

WqxHalBesta::WqxHalBesta(): romFile(nullptr), norFile(nullptr), bbsFile(nullptr), firstOut(0), cacheSize(0),
                            currentMappedNorPage(-1), cacheBlockTable(nullptr), cacheBlock(nullptr), romIndexLow{0},
                            romIndexHigh{0}, norIndexLow{0}, norIndexHigh(0xffffffff), bbsCache{0} {}

bool WqxHalBesta::begin(size_t cacheSize) {
    cacheBlock = reinterpret_cast<CacheBlock *>(lmalloc(cacheSize * sizeof(CacheBlock)));
    cacheBlockTable = reinterpret_cast<CacheBlock **>(lcalloc(1, sizeof(CacheBlock *) * cacheSize));
    std::memset(romIndexLow, 0xff, sizeof(romIndexLow));
    std::memset(romIndexHigh, 0xff, sizeof(romIndexHigh));
    std::memset(norIndexLow, 0xff, sizeof(norIndexLow));
    this->cacheSize = cacheSize;
    return true;
}

unsigned short WqxHalBesta::getNorCacheIndex(uint32_t page) {
    unsigned short result = norIndexLow[page] | ((norIndexHigh >> page) & 1) << 8;
    //Printf("norcache %d -> %#x\n", page, result);
    return result;
}

void WqxHalBesta::setNorCacheIndex(uint32_t page, unsigned short index) {
    //Printf("norcache %d <- %#x\n", page, index);
    uint8_t newHigh = (index >> 8) & 1;
    norIndexLow[page] = index & 0xff;
    norIndexHigh &= ~(1 << page);
    norIndexHigh |= (newHigh << page);
}

unsigned short WqxHalBesta::getRomCacheIndex(uint32_t volume, uint32_t page) {
    uint32_t pageAddress = volume * 0x80 + page;
    uint32_t romIndexHighOffset = pageAddress / 32;
    uint32_t romIndexHighShift = pageAddress % 32;
    unsigned short result = romIndexLow[pageAddress] | ((romIndexHigh[romIndexHighOffset] >> romIndexHighShift) & 1) << 8;
    //Printf("romcache %d %d -> %#x\n", volume, page, result);
    return result;
}

void WqxHalBesta::setRomCacheIndex(uint32_t volume, uint32_t page, unsigned short index) {
    //Printf("romcache %d %d <- %#x\n", volume, page, index);
    uint8_t newHigh = (index >> 8) & 1;
    uint32_t pageAddress = volume * 0x80 + page;
    uint32_t romIndexHighOffset = pageAddress / 32;
    uint32_t romIndexHighShift = pageAddress % 32;
    romIndexLow[pageAddress] = index & 0xff;
    romIndexHigh[romIndexHighOffset] &= ~(1 << romIndexHighShift);
    romIndexHigh[romIndexHighOffset] |= (newHigh << romIndexHighShift);
}

CacheBlock *WqxHalBesta::claimPage(unsigned short cacheIndex) {
    // Claim a previously used or unused page. Will perform eviction when necessary.
    CacheBlock *cachedPage = cacheBlockTable[cacheIndex];
    if (cachedPage == nullptr) {
        //Printf("Index %d is unused\n", cacheIndex);
        // Previously unused. Fix link.
        cacheBlockTable[cacheIndex] = &cacheBlock[cacheIndex];
        cachedPage = cacheBlockTable[cacheIndex];
    } else if (cachedPage->flags >= FLAG_ROM_VOLUME_0 && cachedPage->flags <= FLAG_ROM_VOLUME_2) {
        // Previously used as a ROM page.
        //Printf("Index %d is ROM\n", cacheIndex);
        setRomCacheIndex(cachedPage->flags, cachedPage->page, CACHE_INDEX_UNUSED);
    } else if ((cachedPage->flags & FLAG_NOR) == FLAG_NOR) {
        //Printf("Index %d is ", cacheIndex);
        if (cachedPage->flags & FLAG_NOR_DIRTY) {
            //Printf("uncommitted ");
            // Previously used as a NOR page with uncommitted changes.
            __fseek(norFile, cachedPage->page * 0x8000, _SYS_SEEK_SET);
            _fwrite(cachedPage->data, 1, 0x8000, norFile);
        };
        //Printf("NOR\n");
        setNorCacheIndex(cachedPage->page, CACHE_INDEX_UNUSED);
    } else {
        // Something is wrong. Possibly data corruption or logical error.
        //Printf("Error\n");
        return nullptr;
    }
    return cachedPage;
}

bool WqxHalBesta::loadNorPage(uint32_t page) {
    if (page > 0x1f || !ensureOpen()) {
        return false;
    }

    //Printf("nor %d\n", page);
    unsigned short cached = getNorCacheIndex(page);
    if (cached != CACHE_INDEX_UNUSED && cached >= cacheSize) {
        return false;
    }

    if (cached == CACHE_INDEX_UNUSED) {
        // Cache miss
        cached = firstOut;
        auto claimedPage = claimPage(cached);
        if (claimedPage == nullptr) {
            return false;
        }
        setNorCacheIndex(page, cached);
        claimedPage->flags = FLAG_NOR;
        claimedPage->page = page;
        this->page = claimedPage->data;
        __fseek(norFile, page * 0x8000, _SYS_SEEK_SET);
        if (_fread(this->page, 1, 0x8000, norFile) != 0x8000) {
            return false;
        }
        firstOut++;
        if (firstOut >= cacheSize) {
            firstOut = 0;
        }
    } else {
        // Cache hit
        this->page = cacheBlockTable[cached]->data;
    }

    currentMappedNorPage = page;
    return true;
}

bool WqxHalBesta::saveNorPage(uint32_t page) {
    (void) page;
    if (!ensureOpen()) {
        return false;
    }
    auto cached = getNorCacheIndex(currentMappedNorPage);
    cacheBlockTable[cached]->flags |= FLAG_NOR_DIRTY;
    return true;
}

bool WqxHalBesta::wipeNorFlash() {
    char *fill = reinterpret_cast<char *>(lmalloc(512));
    if (fill == nullptr) {
        return false;
    }

    // Clear all cached NOR pages
    for (uint8_t i=0; i<0x20; i++) {
        auto index = getNorCacheIndex(i);
        if (index != CACHE_INDEX_UNUSED) {
            auto block = cacheBlockTable[index];
            if (block != nullptr && (block->flags & FLAG_NOR) == FLAG_NOR) {
                std::memset(block->data, 0xff, sizeof(block->data));
                block->flags &= ~FLAG_NOR_DIRTY;
            }
        }
    }

    // 0xff fill the NOR flash image
    std::memset(fill, 0xff, 512);
    for (size_t i = 0; i < 2048; i++) {
        _fwrite(fill, 1, 512, norFile);
    }
    _lfree(fill);
    return true;
}

bool WqxHalBesta::loadRomPage(uint32_t volume, uint32_t page) {
    if (page > 0x7f || volume > 2 || !ensureOpen()) {
        return false;
    }

    //Printf("rom %d %d\n", volume, page);
    unsigned short cached = getRomCacheIndex(volume, page);
    if (cached != CACHE_INDEX_UNUSED && cached >= cacheSize) {
        return false;
    }

    if (cached == CACHE_INDEX_UNUSED) {
        // Cache miss
        cached = firstOut;
        auto claimedPage = claimPage(cached);
        if (claimedPage == nullptr) {
            return false;
        }
        setRomCacheIndex(volume, page, cached);
        claimedPage->flags = volume;
        claimedPage->page = page;
        this->page = claimedPage->data;
        __fseek(romFile, (volume * 0x80 + page) * 0x8000, _SYS_SEEK_SET);
        if (_fread(this->page, 1, 0x8000, romFile) != 0x8000) {
            return false;
        }
        firstOut++;
        if (firstOut >= cacheSize) {
            firstOut = 0;
        }
    } else {
        // Cache hit
        this->page = cacheBlockTable[cached]->data;
    }

    return true;
}

bool WqxHalBesta::loadBbsPage(uint32_t volume, uint32_t page) {
    (void) volume;
    if (page > 0xf || volume > 2 || !ensureOpen()) {
        return false;
    }
    this->bbs = &bbsCache[page * 0x2000];
    this->shadowBbs = &bbsCache[0x2000];
    return true;
}

bool WqxHalBesta::saveState(const char *states, size_t size) {
    void *statesFile = _afopen(STATE_FILE, "wb+");
    if (statesFile == nullptr) {
        return false;
    }
    _fwrite(states, 1, size, statesFile);
    _fclose(statesFile);
    return true;
}

bool WqxHalBesta::loadState(char *states, size_t size) {
    void *statesFile = _afopen(STATE_FILE, "rb");
    if (statesFile == nullptr) {
        return false;
    }
    _fread(states, 1, size, statesFile);
    _fclose(statesFile);
    return true;
}

bool WqxHalBesta::ensureOpen() {
    if (norFile == nullptr) {
        norFile = _afopen(NOR_FILE, "rb+");
    }
    if (romFile == nullptr) {
        romFile = _afopen(ROM_FILE, "rb");
    }
    if (bbsFile == nullptr) {
        bbsFile = _afopen(BBS_FILE, "rb");
        _fread(bbsCache, 1, sizeof(bbsCache), bbsFile);
        this->shadowBbs = &bbsCache[0x2000];
    }
    if (norFile == nullptr || romFile == nullptr || bbsFile == nullptr) {
        closeAll();
        return false;
    }
    if (cacheBlock == nullptr || cacheBlockTable == nullptr || this->cacheSize == 0) {
        closeAll();
        return false;
    }
    return true;
}

static WqxHalBesta hal;

void WqxHalBesta::closeAll() {
    // Flush all NOR pages marked as saved
    for (uint8_t i=0; i<0x20; i++) {
        auto index = getNorCacheIndex(i);
        if (index != CACHE_INDEX_UNUSED) {
            auto block = cacheBlockTable[index];
            if (block != nullptr && block->flags & FLAG_NOR_DIRTY) {
                __fseek(norFile, block->page * 0x8000, _SYS_SEEK_SET);
                _fwrite(block->data, 1, 0x8000, norFile);
                block->flags &= ~FLAG_NOR_DIRTY;
            }
        }
    }
    if (norFile != nullptr) {
        _fclose(norFile);
        norFile = nullptr;
    }
    if (romFile != nullptr) {
        _fclose(romFile);
        romFile = nullptr;
    }
    if (bbsFile != nullptr) {
        _fclose(bbsFile);
        bbsFile = nullptr;
    }
    if (cacheBlock != nullptr) {
        _lfree(cacheBlock);
        cacheBlock = nullptr;
    }
    if (cacheBlockTable != nullptr) {
        _lfree(cacheBlockTable);
        cacheBlockTable = nullptr;
    }
}

event_t *ticker_event = nullptr;
short pressing0 = 0, pressing1 = 0;

static inline bool test_events_no_shift(ui_event_t *uievent) {
    // Deactivate shift key because it may cause the keycode to change.
    // This means we need to handle shift behavior ourselves (if we really need it) but that's a fair tradeoff.
    SetShiftState(TOGGLE_KEY_INACTIVE);
    return TestPendEvent(uievent) || TestKeyEvent(uievent);
}

void ext_ticker() {
    static auto uievent = ui_event_t();
    bool hit = false;

    // TODO this still seem to lose track presses on BA110. Find out why.
    while (test_events_no_shift(&uievent)) {
        hit = true;
        if (GetEvent(&uievent) && uievent.event_type == 0x10) {
            pressing0 = uievent.key_code0;
            pressing1 = uievent.key_code1;
        } else {
            ClearEvent(&uievent);
        }
    }

    if (!hit) {
        pressing0 = 0;
        pressing1 = 0;
    }

    OSSetEvent(ticker_event);
}

void drain_all_events() {
    auto uievent = ui_event_t();
    size_t silence_count = 0;
    while (silence_count < 60) {
        bool test = (TestPendEvent(&uievent) || TestKeyEvent(&uievent));
        if (test) {
            ClearAllEvents();
            silence_count = 0;
        }
        OSSleep(1);
        silence_count++;
    }
}

short map_key_binding(short key) {
    if (key >= KEY_ESC && key <= KEY_PGDN) {
        return KEYMAP_0x01[key - KEY_ESC];
    } else if (key >= KEY_A && key <= KEY_Z) {
        return KEYMAP_ALPHABETS[key - KEY_A];
    } else if (key >= KEY_0 && key <= KEY_9) {
        return KEYMAP_NUMBERS[key - KEY_0];
    } else {
        switch (key) {
        case KEY_SPACE:
            return 0x3e; // space
        case KEY_ENTER:
            return 0x1d; // enter
        case KEY_FONT:
        case KEY_DOT:
            return 0x3d; // .
        case KEY_HELP:
        case KEY_SAVE:
            return 0x38; // help
        case KEY_SHIFT:
            return 0x39; // shift
        case KEY_TAB:
            return 0x3a; // caps (Besta shift modifier is not supported yet)
        case KEY_MENU:
            return 0x0e; // download
        case KEY_SYMBOL:
        case KEY_SEARCH:
            return 0x3c; // symbol/0
        }
    }
    return -1;
}

int main() {
    auto old_hold_cfg = key_press_event_config_t();
    uint32_t quit_ticks = 0;

    rgbSetBkColor(0xffffff);
    ClearScreen(false);

    // Parse config file
    auto cpu_speed = _GetPrivateProfileInt("Hacks", "CPUSpeed", 0, CONFIG_FILE);
    auto cache_size_conf = _GetPrivateProfileInt("Hacks", "CacheSizeLimit", 0, CONFIG_FILE);

    ticker_event = OSCreateEvent(0, 0);

    size_t allocSize = GetImageSizeExt(160, 80, 1);
    lcd_surface_t *fb = reinterpret_cast<lcd_surface_t *>(lcalloc(1, allocSize));
    InitGraphic(fb, 160, 80, 1);

    lcd_t *lcd = GetActiveLCD();
    short offsetx = (lcd->width - fb->width) / 2;
    short offsety = (lcd->height - fb->height) / 2;

    if (fb->palette != nullptr) {
        fb->palette[0] = 0xffffff;
        fb->palette[1] = 0x000000;
    }

    size_t heap_space = GetFreeMemory();
    if (heap_space <= HEAP_RESERVED) {
        _lfree(fb);
        return 1;
    }
    size_t allowed_cache_size = (heap_space - HEAP_RESERVED) / CACHE_OVERHEAD_UNIT;
    // Not enough memory
    if (allowed_cache_size == 0) {
        _lfree(fb);
        return 1;
    }

    // Allocate the cache
    size_t final_cache_size;
    if (cache_size_conf == 0) {
        final_cache_size = (allowed_cache_size > MAX_CACHE_SIZE) ? MAX_CACHE_SIZE : allowed_cache_size;
    } else {
        final_cache_size = (cache_size_conf > MAX_CACHE_SIZE) ? MAX_CACHE_SIZE : cache_size_conf;
    }

    hal.begin(final_cache_size);

    if (!hal.ensureOpen()) {
        _lfree(fb);
        return 1;
    }

    wqx::Initialize(&hal, cpu_speed);
    wqx::LoadNC1020();

    // Set up "spam key press as key down" handler
    GetSysKeyState(&old_hold_cfg);
    SetTimer1IntHandler(&ext_ticker, 3);
    SetSysKeyState(&KEY_EVENT_CONFIG_TURBO);

    while (true) {
        if (OSWaitForEvent(ticker_event, 10000) != WAIT_RESULT_RESOLVED) {
            OSCloseEvent(ticker_event);
            hal.closeAll();
            _lfree(fb);
            return 1;
        }

        // TODO draw a bar for the tick counter
        if (pressing0 == KEY_HOME) {
            quit_ticks++;
            // 20 (~600ms) seems to be (somewhat) reliable. More than this and the quit condition may never be
            // triggered on BA110.
            if (quit_ticks >= 20) {
                break;
            }
        } else {
            quit_ticks = 0;
        }

        // Handle key presses
        // TODO handle pressing1 as well
        short target_key_code = map_key_binding(pressing0);
        if (target_key_code >= 0) {
            wqx::SetKey(target_key_code, true);
        } else {
            wqx::ReleaseAllKeys();
        }

        // Run emulator and draw LCD
        wqx::RunTimeSlice(30, false);
        wqx::CopyLcdBuffer(reinterpret_cast<uint8_t *>(fb->buffer));
        // TODO handle the LCD graphic segments (the 7seg counter, icons, scroll bar, etc.)
        ShowGraphic(offsetx, offsety, fb, BLIT_NONE);
    }

    // Drain all problematic events that might raise and revert to normal key press behavior
    SetSysKeyState(&KEY_EVENT_CONFIG_DRAIN);
    SetTimer1IntHandler(NULL, 0);
    drain_all_events();
    SetSysKeyState(&old_hold_cfg);

    wqx::SaveNC1020();
    OSCloseEvent(ticker_event);
    hal.closeAll();
    _lfree(fb);
    return 0;
}
