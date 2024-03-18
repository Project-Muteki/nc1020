#include "nc1020.h"

#include <cstring>
#include <cstdint>
#include <muteki/file.h>
#include <muteki/memory.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>

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
    void *statesFile = _afopen("C:\\nc1020.sts", "wb+");
    if (statesFile == nullptr) {
        return false;
    }
    _fwrite(states, 1, size, statesFile);
    _fclose(statesFile);
    return true;
}

bool WqxHalBesta::loadState(char *states, size_t size) {
    void *statesFile = _afopen("C:\\nc1020.sts", "rb");
    if (statesFile == nullptr) {
        return false;
    }
    _fread(states, 1, size, statesFile);
    _fclose(statesFile);
    return true;
}

bool WqxHalBesta::ensureOpen() {
    if (norFile == nullptr) {
        norFile = _afopen("C:\\nor.bin", "rb+");
    }
    if (romFile == nullptr) {
        romFile = _afopen("C:\\rom.bin", "rb");
    }
    if (bbsFile == nullptr) {
        bbsFile = _afopen("C:\\bbs.bin", "rb");
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

int main() {
    rgbSetBkColor(0xffffff);
    ClearScreen(false);
    
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

    ui_event_t uievent = {
        .unk0 = 0,
        .event_type = 0,
        .key_code0 = 0,
        .key_code1 = 0,
        .usb_data = nullptr,
        .unk16 = nullptr,
        .unk20 = nullptr,
    };

    while (true) {
        // TODO fix the timing here
        wqx::RunTimeSlice(33, false);
        if ((TestPendEvent(&uievent) || TestKeyEvent(&uievent)) && GetEvent(&uievent)) {
            if (uievent.key_code0 == KEY_ESC) {
                break;
            }
        }
        wqx::CopyLcdBuffer(reinterpret_cast<uint8_t *>(fb->buffer));
        ShowGraphic(offsetx, offsety, fb, BLIT_NONE);
    }

    wqx::SaveNC1020();
    hal.closeAll();
    _lfree(fb);
    return 0;
}
