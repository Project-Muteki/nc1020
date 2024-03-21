#ifndef NC1020_H_
#define NC1020_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
namespace wqx {
/**
 * @brief Interface for HAL.
 * @details
 * Implement this for your OS/board and pass an instance to Initialize() to get started.
 */
class IWqxHal {
public:
    /**
     * @brief Page scratch pad buffer (0x8000 bytes long).
     */
    uint8_t *page;
    /**
     * @brief BBS scratch pad buffer (0x2000 bytes long).
     */
    uint8_t *bbs;

    /**
     * @brief Shadowed BBS scratch pad buffer (0x2000 bytes long).
     */
    uint8_t *shadowBbs;

    IWqxHal();
    /**
     * @brief Map a page from NOR flash image to the scratch pad buffer IWqxHal::page.
     * @param page Source page number. Must be between `0x00` and `0x20` (inclusive-exclusive).
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool loadNorPage(uint32_t page) = 0;
    /**
     * @brief Save the current scratch pad buffer at IWqxHal::page to NOR flash image.
     * @param page Destination page number. Must be between `0x00` and `0x20` (inclusive-exclusive).
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool saveNorPage(uint32_t page) = 0;
    /**
     * @brief Erase NOR flash page.
     * @details The page should be filled with 0xff after erasure.
     * @param page Page number. Must be between `0x00` and `0x20` (inclusive-exclusive).
     * @param count Number of pages to erase.
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool eraseNorPage(uint32_t page, uint32_t count) = 0;
    /**
     * @brief Map a page from mask ROM image to the scratch pad buffer IWqxHal::page.
     * @note GGV NC1020 defines page numbers starting from `0x80` as the ROM page number. The `page` parameter here
     * however accepts a page number starting from 0. So instead of passing `0x80`, `0x81`, ... as the page number, 
     * one should pass `0x00`, `0x01`, ... instead to get the same effect.
     * @param volume Volume index. Volume is defined as the last 4MiB of the 8MiB chunks within the simulator ROM
     * image. Must be between `0` and `3` (inclusive-exclusive).
     * @param page Page number. Must be between `0x00` and `0x80` (inclusive-exclusive).
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool loadRomPage(uint32_t volume, uint32_t page) = 0;
    /**
     * @brief Map a page from BBS ROM image to the scratch pad buffer IWqxHal::bbs.
     * @param volume Volume index. Volume is defined as the first 128KiB of the 8MiB chunks within the simulator ROM
     * image. Must be between `0` and `3` (inclusive-exclusive).
     * @param page Page number. Must be between `0x00` and `0x10` (inclusive-exclusive).
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool loadBbsPage(uint32_t volume, uint32_t page) = 0;
    /**
     * @brief Save emulator states to persistent storage.
     * @param[in] states Serialized emulator states.
     * @param size Size of serialized emulator states.
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool saveState(const char *states, size_t size) = 0;
    /**
     * @brief Load emulator states from persistent storage.
     * @details This function passes through the data only and should not perform checks on the data. Although it may
     * check whether `size` is small enough for the storage backend to hold the data.
     * @param[out] states Serialized emulator states.
     * @param size Size of serialized emulator states.
     * @retval true Success.
     * @retval false Failure.
     */
    virtual bool loadState(char *states, size_t size) = 0;
};

extern void Initialize(IWqxHal *);
extern void Reset();
extern void SetKey(uint8_t, bool);
extern void ReleaseAllKeys();
extern void RunTimeSlice(uint32_t, bool);
extern bool CopyLcdBuffer(uint8_t*);
extern void LoadNC1020();
extern void SaveNC1020();
}

#endif /* NC1020_H_ */
