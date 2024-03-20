#include "nc1020.h"

#include <cstring>
#include <cstdint>
#include <muteki/file.h>
#include <muteki/memory.h>
#include <muteki/system.h>
#include <muteki/threading.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>

const key_press_event_config_t KEY_EVENT_CONFIG_DRAIN = {65535, 65535, 1};
const key_press_event_config_t KEY_EVENT_CONFIG_TURBO = {0, 0, 0};

const char ROM_FILE = "C:\\rom.bin";
const char NOR_FILE = "C:\\nor.bin";
const char BBS_FILE = "C:\\bbs.bin";
const char STATE_FILE = "C:\\nc1020.sts";

const uint8_t KEYMAP_ARROWS[4] = {0x3f, 0x1a, 0x1f, 0x1b}; // KEY_LEFT - KEY_DOWN

class WqxHalBesta : public wqx::IWqxHal {
public:
    WqxHalBesta();
    virtual bool loadNorPage(uint32_t page) override;
    virtual bool saveNorPage(uint32_t page) override;
    virtual bool eraseNorPage(uint32_t page, uint32_t count) override;
    virtual bool loadRomPage(uint32_t volume, uint32_t page) override;
    virtual bool loadBbsPage(uint32_t volume, uint32_t page) override;
    virtual bool saveState(const char *states, size_t size) override;
    virtual bool loadState(char *states, size_t size) override;
    void closeAll();
    bool ensureOpen();
private:
    void *romFile;
    void *norFile;
    void *bbsFile;
};

WqxHalBesta::WqxHalBesta(): romFile(nullptr), norFile(nullptr) {}

bool WqxHalBesta::loadNorPage(uint32_t page) {
    if (page > 0x1f || !ensureOpen()) {
        return false;
    }
    __fseek(norFile, page * 0x8000, _SYS_SEEK_SET);
    if (_fread(this->page, 1, sizeof(this->page), norFile) != sizeof(this->page)) {
        return false;
    }
    return true;
}

bool WqxHalBesta::saveNorPage(uint32_t page) {
    if (page > 0x1f || !ensureOpen()) {
        return false;
    }
    __fseek(norFile, page * 0x8000, _SYS_SEEK_SET);
    _fwrite(this->page, 1, sizeof(this->page), norFile);
    //__fflush(norFile);
    return true;
}

bool WqxHalBesta::eraseNorPage(uint32_t page, uint32_t count) {
    if ((page > 0x1f && (count > 0x20 - page)) || !ensureOpen()) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    __fseek(norFile, page * 0x8000, _SYS_SEEK_SET);
    memset(this->page, 0xff, sizeof(this->page));
    for (size_t i = 0; i < count; i++) {
        _fwrite(this->page, 1, sizeof(this->page), norFile);
    }
    //__fflush(norFile);
    return true;
}

bool WqxHalBesta::loadRomPage(uint32_t volume, uint32_t page) {
    if (page > 0x7f || volume > 2 || !ensureOpen()) {
        return false;
    }
    __fseek(romFile, (volume * 0x80 + page) * 0x8000, _SYS_SEEK_SET);
    if (_fread(this->page, 1, sizeof(this->page), romFile) != sizeof(this->page)) {
        return false;
    }
    return true;
}

bool WqxHalBesta::loadBbsPage(uint32_t volume, uint32_t page) {
    (void) volume;
    if (page > 0x0f || !ensureOpen()) {
        return false;
    }
    __fseek(bbsFile, page * 0x2000, _SYS_SEEK_SET);
    if (_fread(this->bbs, 1, sizeof(this->bbs), bbsFile) != sizeof(this->bbs)) {
        return false;
    }
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
    }
    if (norFile == nullptr || romFile == nullptr || bbsFile == nullptr) {
        closeAll();
        return false;
    }
    return true;
}

static WqxHalBesta hal;

void WqxHalBesta::closeAll() {
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
    if (key >= KEY_LEFT && key <= KEY_DOWN) {
        return KEYMAP_ARROWS[key - KEY_LEFT];
    }
    return -1;
}

int main() {
    auto old_hold_cfg = key_press_event_config_t();

    rgbSetBkColor(0xffffff);
    ClearScreen(false);

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

    if (!hal.ensureOpen()) {
        return 1;
    }

    wqx::Initialize(&hal);
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

        // TODO Change this to a less useful key
        if (pressing0 == KEY_ESC) {
            break;
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
