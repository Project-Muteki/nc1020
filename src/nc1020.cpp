#include "nc1020.h"
#include <string>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace wqx {
    using std::string;
    
    // default cpu cycles per second (cpu freq).
    const uint32_t CYCLES_SECOND = 5120000;
    const uint32_t TIMER0_FREQ = 2;
    const uint32_t TIMER1_FREQ = 0x100;
    
    static const uint32_t ROM_SIZE = 0x8000 * 0x300;
    static const uint32_t NOR_SIZE = 0x8000 * 0x20;
    
    static const uint16_t IO_LIMIT = 0x40;
#define IO_API
    typedef uint8_t (IO_API *io_read_func_t)(uint8_t);
    typedef void (IO_API *io_write_func_t)(uint8_t, uint8_t);
    
    const uint16_t NMI_VEC = 0xFFFA;
    const uint16_t RESET_VEC = 0xFFFC;
    const uint16_t IRQ_VEC = 0xFFFE;
    
    const uint32_t VERSION = 0x06;

    // Runtime timing settings
    // cpu cycles per timer0 period (1/2 s).
    uint32_t cycles_timer0 = 0;
    // cpu cycles per timer1 period (1/256 s).
    uint32_t cycles_timer1 = 0;
    // speed up
    uint32_t cycles_timer1_speed_up = 0;
    // cpu cycles per ms (1/1000 s).
    uint32_t cycles_ms = 0;

typedef struct {
	uint16_t reg_pc;
	uint8_t reg_a;
	uint8_t reg_ps;
	uint8_t reg_x;
	uint8_t reg_y;
	uint8_t reg_sp;
} cpu_states_t;

struct nc1020_states_t {
	uint32_t version;
	cpu_states_t cpu;
	uint8_t ram[0x8000];

	uint8_t bak_40[0x40];

	uint8_t clock_data[80];
	uint8_t clock_flags;

	uint8_t jg_wav_data[0x20];
	uint8_t jg_wav_flags;
	uint8_t jg_wav_idx;
	bool jg_wav_playing;

	uint8_t fp_step;
	uint8_t fp_type;
	uint8_t fp_bank_idx;
	uint8_t fp_bak1;
	uint8_t fp_bak2;
	uint8_t fp_buff[0x100];

	bool slept;
	bool should_wake_up;
	bool pending_wake_up;
	uint8_t wake_up_flags;

	bool timer0_toggle;
	uint32_t cycles;
	uint32_t timer0_cycles;
	uint32_t timer1_cycles;
	bool should_irq;

	uint32_t lcd_addr;
	uint8_t keypad_matrix[8];
};

static IWqxHal *hal = nullptr;

static uint8_t* memmap[8];
static nc1020_states_t nc1020_states;

static uint32_t& version = nc1020_states.version;

static uint16_t& reg_pc = nc1020_states.cpu.reg_pc;
static uint8_t& reg_a = nc1020_states.cpu.reg_a;
static uint8_t& reg_ps = nc1020_states.cpu.reg_ps;
static uint8_t& reg_x = nc1020_states.cpu.reg_x;
static uint8_t& reg_y = nc1020_states.cpu.reg_y;
static uint8_t& reg_sp = nc1020_states.cpu.reg_sp;

static uint8_t* ram_buff = nc1020_states.ram;
static uint8_t* stack = ram_buff + 0x100;
static uint8_t* ram_io = ram_buff;
static uint8_t* ram_40 = ram_buff + 0x40;
static uint8_t* ram_page0 = ram_buff;
static uint8_t* ram_page1 = ram_buff + 0x2000;
static uint8_t* ram_page2 = ram_buff + 0x4000;
static uint8_t* ram_page3 = ram_buff + 0x6000;

static uint8_t* clock_buff = nc1020_states.clock_data;
static uint8_t& clock_flags = nc1020_states.clock_flags;

static uint8_t* jg_wav_buff = nc1020_states.jg_wav_data;
static uint8_t& jg_wav_flags = nc1020_states.jg_wav_flags;
static uint8_t& jg_wav_index = nc1020_states.jg_wav_idx;
static bool& jg_wav_playing = nc1020_states.jg_wav_playing;

static uint8_t* bak_40 = nc1020_states.bak_40;
static uint8_t& fp_step = nc1020_states.fp_step;
static uint8_t& fp_type = nc1020_states.fp_type;
static uint8_t& fp_bank_idx = nc1020_states.fp_bank_idx;
static uint8_t& fp_bak1 = nc1020_states.fp_bak1;
static uint8_t& fp_bak2 = nc1020_states.fp_bak2;
static uint8_t* fp_buff = nc1020_states.fp_buff;

static bool& slept = nc1020_states.slept;
static bool& should_wake_up = nc1020_states.should_wake_up;
static bool& wake_up_pending = nc1020_states.pending_wake_up;
static uint8_t& wake_up_key = nc1020_states.wake_up_flags;

static bool& should_irq = nc1020_states.should_irq;
static bool& timer0_toggle = nc1020_states.timer0_toggle;
static uint32_t& cycles = nc1020_states.cycles;
static uint32_t& timer0_cycles = nc1020_states.timer0_cycles;
static uint32_t& timer1_cycles = nc1020_states.timer1_cycles;

static uint8_t* keypad_matrix = nc1020_states.keypad_matrix;
static uint32_t& lcd_addr = nc1020_states.lcd_addr;

static io_read_func_t io_read[0x40];
static io_write_func_t io_write[0x40];

IWqxHal::IWqxHal() : page{0}, bbs{0} {}

uint8_t* GetBank(uint8_t bank_idx){
	uint8_t volume_idx = ram_io[0x0D] & 0x0f;
    if (bank_idx < 0x20) {
        hal->loadNorPage(bank_idx);
    	return hal->page;
    } else if (bank_idx >= 0x80) {
        hal->loadRomPage(volume_idx, bank_idx - 0x80);
        return hal->page;
    }
    return NULL;
}

void SwitchBank(){
	uint8_t bank_idx = ram_io[0x00];
	uint8_t* bank = GetBank(bank_idx);
    memmap[2] = bank;
    memmap[3] = bank + 0x2000;
    memmap[4] = bank + 0x4000;
    memmap[5] = bank + 0x6000;
}

void SwitchVolume(){
	uint8_t volume_idx = ram_io[0x0D];
	volume_idx = volume_idx > 2 ? 0 : volume_idx;

    // Load normal bbs (except when hitting the shadowed page then we map ram_page3) to 0xc000 and shadowed bbs to 0xe000
    uint8_t roa_bbs = ram_io[0x0A] & 0x0f;
    memmap[1] = (roa_bbs & 0x04 ? ram_page2 : ram_page1);
    if (volume_idx == 0 && roa_bbs == 1) {
        memmap[6] = ram_page3;
    } else {
        hal->loadBbsPage(volume_idx, roa_bbs);
        memmap[6] = hal->bbs;
    }
    memmap[7] = hal->shadowBbs;

    SwitchBank();
}

void GenerateAndPlayJGWav(){

}

uint8_t* GetPtr40(uint8_t index){
    if (index < 4) {
        return ram_io;
    } else {
        return ram_buff + ((index) << 6);
    }
}

uint8_t IO_API ReadXX(uint8_t addr){
	return ram_io[addr];
}

uint8_t IO_API Read06(uint8_t addr){
	return ram_io[addr];
}

uint8_t IO_API Read3B(uint8_t addr){
    if (!(ram_io[0x3D] & 0x03)) {
        return clock_buff[0x3B] & 0xFE;
    }
    return ram_io[addr];
}

uint8_t IO_API Read3F(uint8_t addr){
    (void) addr;
    uint8_t idx = ram_io[0x3E];
    return idx < 80 ? clock_buff[idx] : 0;
}

void IO_API WriteXX(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
}


// switch bank.
void IO_API Write00(uint8_t addr, uint8_t value){
    uint8_t old_value = ram_io[addr];
    ram_io[addr] = value;
    if (value != old_value) {
    	SwitchBank();
    }
}

void IO_API Write05(uint8_t addr, uint8_t value){
	uint8_t old_value = ram_io[addr];
	ram_io[addr] = value;
	if ((old_value ^ value) & 0x08) {
		slept = !(value & 0x08);
	}
}

void IO_API Write06(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    if (!lcd_addr) {
    	lcd_addr = ((ram_io[0x0C] & 0x03) << 12) | (value << 4);
    }
    ram_io[0x09] &= 0xFE;
}

void IO_API Write08(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    ram_io[0x0B] &= 0xFE;
}

// keypad matrix.
void IO_API Write09(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    switch (value){
    case 0x01: ram_io[0x08] = keypad_matrix[0]; break;
    case 0x02: ram_io[0x08] = keypad_matrix[1]; break;
    case 0x04: ram_io[0x08] = keypad_matrix[2]; break;
    case 0x08: ram_io[0x08] = keypad_matrix[3]; break;
    case 0x10: ram_io[0x08] = keypad_matrix[4]; break;
    case 0x20: ram_io[0x08] = keypad_matrix[5]; break;
    case 0x40: ram_io[0x08] = keypad_matrix[6]; break;
    case 0x80: ram_io[0x08] = keypad_matrix[7]; break;
    case 0:
        ram_io[0x0B] |= 1;
        if (keypad_matrix[7] == 0xFE) {
            ram_io[0x0B] &= 0xFE;
        }
        break;
    case 0x7F:
        if (ram_io[0x15] == 0x7F) {
            ram_io[0x08] = (
                keypad_matrix[0] |
                keypad_matrix[1] |
                keypad_matrix[2] |
                keypad_matrix[3] |
                keypad_matrix[4] |
                keypad_matrix[5] |
                keypad_matrix[6] |
                keypad_matrix[7]
            );
        }
        break;
    }
}

// roabbs
void IO_API Write0A(uint8_t addr, uint8_t value){
    uint8_t old_value = ram_io[addr];
    ram_io[addr] = value;
    if (value != old_value) {
        uint8_t volume_idx = ram_io[0x0D];
        volume_idx = volume_idx > 2 ? 0 : volume_idx;
        hal->loadBbsPage(volume_idx, value & 0x0f);
        memmap[6] = hal->bbs;
    }
}

// switch volume
void IO_API Write0D(uint8_t addr, uint8_t value){
	uint8_t old_value = ram_io[addr];
    ram_io[addr] = value;
    if (value != old_value) {
        SwitchVolume();
    }
}

// zp40 switch
void IO_API Write0F(uint8_t addr, uint8_t value){
	uint8_t old_value = ram_io[addr];
    ram_io[addr] = value;
    old_value &= 0x07;
    value &= 0x07;
    if (value != old_value) {
        uint8_t* ptr_new = GetPtr40(value);
        if (old_value) {
            memcpy(GetPtr40(old_value), ram_40, 0x40);
            memcpy(ram_40, value ? ptr_new : bak_40, 0x40);
        } else {
            memcpy(bak_40, ram_40, 0x40);
            memcpy(ram_40, ptr_new, 0x40);
        }
    }
}

void IO_API Write20(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    if (value == 0x80 || value == 0x40) {
        memset(jg_wav_buff, 0, 0x20);
        ram_io[0x20] = 0;
        jg_wav_flags = 1;
        jg_wav_index = 0;
    }
}

void IO_API Write23(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    if (value == 0xC2) {
        jg_wav_buff[jg_wav_index] = ram_io[0x22];
    } else if (value == 0xC4) {
        if (jg_wav_index < 0x20) {
            jg_wav_buff[jg_wav_index] = ram_io[0x22];
            jg_wav_index ++;
        }
    } else if (value == 0x80) {
        ram_io[0x20] = 0x80;
        jg_wav_flags = 0;
        if (jg_wav_index) {
            if (!jg_wav_playing) {
                GenerateAndPlayJGWav();
                jg_wav_index = 0;
            }
        }
    }
    if (jg_wav_playing) {
        // todo.
    }
}

// clock.
void IO_API Write3F(uint8_t addr, uint8_t value){
    ram_io[addr] = value;
    uint8_t idx = ram_io[0x3E];
    if (idx >= 0x07) {
        if (idx == 0x0B) {
            ram_io[0x3D] = 0xF8;
            clock_flags |= value & 0x07;
            clock_buff[0x0B] = value ^ ((clock_buff[0x0B] ^ value) & 0x7F);
        } else if (idx == 0x0A) {
            clock_flags |= value & 0x07;
            clock_buff[0x0A] = value;
        } else {
            clock_buff[idx % 80] = value;
        }
    } else {
        if (!(clock_buff[0x0B] & 0x80) && idx < 80) {
            clock_buff[idx] = value;
        }
    }
}

void AdjustTime(){
    if (++ clock_buff[0] >= 60) {
        clock_buff[0] = 0;
        if (++ clock_buff[1] >= 60) {
            clock_buff[1] = 0;
            if (++ clock_buff[2] >= 24) {
                clock_buff[2] &= 0xC0;
                ++ clock_buff[3];
            }
        }
    }
}

bool IsCountDown(){
    if (!(clock_buff[10] & 0x02) ||
        !(clock_flags & 0x02)) {
        return false;
    }
    return (
        ((clock_buff[7] & 0x80) && !(((clock_buff[7] ^ clock_buff[2])) & 0x1F)) ||
        ((clock_buff[6] & 0x80) && !(((clock_buff[6] ^ clock_buff[1])) & 0x3F)) ||
        ((clock_buff[5] & 0x80) && !(((clock_buff[5] ^ clock_buff[0])) & 0x3F))
        );
}

inline uint8_t & Peek(uint8_t addr) {
	return ram_buff[addr];
}
inline uint8_t & Peek(uint16_t addr) {
	return memmap[addr >> 13][addr & 0x1FFF];
}
inline uint16_t PeekW(uint16_t addr) {
	return Peek(addr) | (Peek((uint16_t) (addr + 1)) << 8);
}
inline uint8_t Load(uint16_t addr) {
	if (addr < IO_LIMIT) {
		return io_read[addr](addr);
	}
	if (((fp_step == 4 && fp_type == 2) ||
		(fp_step == 6 && fp_type == 3)) &&
		(addr >= 0x4000 && addr < 0xC000)) {
		fp_step = 0;
		return 0x88;
	}
	if (addr == 0x45F && wake_up_pending) {
		wake_up_pending = false;
		memmap[0][0x45F] = wake_up_key;
	}
	return Peek(addr);
}
inline void Store(uint16_t addr, uint8_t value) {
	if (addr < IO_LIMIT) {
		io_write[addr](addr, value);
		return;
	}
	if (addr < 0x4000) {
		Peek(addr) = value;
		return;
	}
	uint8_t* page = memmap[addr >> 13];
	if (page == ram_page2 || page == ram_page3) {
		page[addr & 0x1FFF] = value;
		return;
	}
	if (addr >= 0xE000) {
		return;
	}

    // write to nor_flash address space.
    // there must select a nor_bank.

    uint8_t bank_idx = ram_io[0x00];
    if (bank_idx >= 0x20) {
        return;
    }

    uint8_t* bank = hal->page;

    if (fp_step == 0) {
        if (addr == 0x5555 && value == 0xAA) {
            fp_step = 1;
        }
        return;
    }
    if (fp_step == 1) {
        if (addr == 0xAAAA && value == 0x55) {
        	fp_step = 2;
            return;
        }
    } else if (fp_step == 2) {
        if (addr == 0x5555) {
        	switch (value) {
        	case 0x90: fp_type = 1; break;
        	case 0xA0: fp_type = 2; break;
        	case 0x80: fp_type = 3; break;
        	case 0xA8: fp_type = 4; break;
        	case 0x88: fp_type = 5; break;
        	case 0x78: fp_type = 6; break;
        	}
            if (fp_type) {
                if (fp_type == 1) {
                    fp_bank_idx = bank_idx;
                    fp_bak1 = bank[0x4000];
                    fp_bak1 = bank[0x4001];
                }
                fp_step = 3;
                return;
            }
        }
    } else if (fp_step == 3) {
        if (fp_type == 1) {
            if (value == 0xF0) {
                bank[0x4000] = fp_bak1;
                bank[0x4001] = fp_bak2;
                hal->saveNorPage(bank_idx);
                fp_step = 0;
                return;
            }
        } else if (fp_type == 2) {
            bank[addr - 0x4000] &= value;
            hal->saveNorPage(bank_idx);
            fp_step = 4;
            return;
        } else if (fp_type == 4) {
            fp_buff[addr & 0xFF] &= value;
            fp_step = 4;
            return;
        } else if (fp_type == 3 || fp_type == 5) {
            if (addr == 0x5555 && value == 0xAA) {
                fp_step = 4;
                return;
            }
        }
    } else if (fp_step == 4) {
        if (fp_type == 3 || fp_type == 5) {
            if (addr == 0xAAAA && value == 0x55) {
                fp_step = 5;
                return;
            }
        }
    } else if (fp_step == 5) {
        // Nuke the entire flash (and optionally NVRAM)
        if (addr == 0x5555 && value == 0x10) {
            hal->wipeNorFlash();
            if (fp_type == 5) {
                memset(fp_buff, 0xFF, 0x100);
            }
            fp_step = 6;
            return;
        }
        if (fp_type == 3) {
            if (value == 0x30) {
                memset(bank + (addr - (addr % 0x800) - 0x4000), 0xFF, 0x800);
                hal->saveNorPage(bank_idx);
                fp_step = 6;
                return;
            }
        } else if (fp_type == 5) {
            if (value == 0x48) {
                memset(fp_buff, 0xFF, 0x100);
                fp_step = 6;
                return;
            }
        }
    }
    if (addr == 0x8000 && value == 0xF0) {
        fp_step = 0;
        return;
    }
    //printf("error occurs when operate in flash!");
}

void Initialize(IWqxHal *halImpl, uint32_t cpu_speed_override) {
	hal = halImpl;
	for (uint32_t i=0; i<0x40; i++) {
		io_read[i] = ReadXX;
		io_write[i] = WriteXX;
	}
	io_read[0x06] = Read06;
	io_read[0x3B] = Read3B;
	io_read[0x3F] = Read3F;
	io_write[0x00] = Write00;
	io_write[0x05] = Write05;
	io_write[0x06] = Write06;
	io_write[0x08] = Write08;
	io_write[0x09] = Write09;
	io_write[0x0A] = Write0A;
	io_write[0x0D] = Write0D;
	io_write[0x0F] = Write0F;
	io_write[0x20] = Write20;
	io_write[0x23] = Write23;
	io_write[0x3F] = Write3F;

        uint32_t cpu_speed = (cpu_speed_override == 0) ? CYCLES_SECOND : cpu_speed_override;
        cycles_timer0 = cpu_speed / TIMER0_FREQ;
        // cpu cycles per timer1 period (1/256 s).
        cycles_timer1 = cpu_speed / TIMER1_FREQ;
        // speed up
        cycles_timer1_speed_up = cpu_speed / TIMER1_FREQ / 20;
        // cpu cycles per ms (1/1000 s).
        cycles_ms = cpu_speed / 1000;

//#ifdef DEBUG
//	FILE* file = fopen((nc1020_dir + "/wqxsimlogs.bin").c_str(), "rb");
//	fseek(file, 0L, SEEK_END);
//	uint32_t file_size = ftell(file);
//	uint32_t insts_count = (file_size - 8) / 8;
//	debug_logs.insts_count = insts_count;
//	debug_logs.logs = (log_rec_t*)malloc(insts_count * 8);
//	fseek(file, 0L, SEEK_SET);
//	fread(&debug_logs.insts_start, 4, 1, file);
//	fseek(file, 4L, SEEK_SET);
//	fread(&debug_logs.peek_addr, 4, 1, file);
//	fseek(file, 8L, SEEK_SET);
//	fread(debug_logs.logs, 8, insts_count, file);
//	fclose(file);
//#endif
}

void ResetStates(){
	version = VERSION;

	memset(ram_buff, 0, 0x8000);
	memmap[0] = ram_page0;
	memmap[2] = ram_page2;
	SwitchVolume();

	memset(keypad_matrix, 0, 8);

	memset(clock_buff, 0, 80);
	clock_flags = 0;

	timer0_toggle = false;

	memset(jg_wav_buff, 0, 0x20);
	jg_wav_flags = 0;
	jg_wav_index = 0;

	should_wake_up = false;
	wake_up_pending = false;

	memset(fp_buff, 0, 0x100);
	fp_step = 0;

	should_irq = false;

	cycles = 0;
	reg_a = 0;
	reg_ps = 0x24;
	reg_x = 0;
	reg_y = 0;
	reg_sp = 0xFF;
	reg_pc = PeekW(RESET_VEC);
	timer0_cycles = cycles_timer0;
	timer1_cycles = cycles_timer1;

//#ifdef DEBUG
//	executed_insts = 0;
//	debug_done = false;
//#endif
}

void Reset() {
	ResetStates();
}

void LoadStates(){
	ResetStates();
        hal->loadState(reinterpret_cast<char *>(&nc1020_states), sizeof(nc1020_states));
	if (version != VERSION) {
		return;
	}
	SwitchVolume();
}

void SaveStates(){
	hal->saveState(reinterpret_cast<const char *>(&nc1020_states), sizeof(nc1020_states));
}

void LoadNC1020(){
	LoadStates();
}

void SaveNC1020(){
	SaveStates();
}

void SetKey(uint8_t key_id, bool down_or_up){
	uint8_t row = key_id % 8;
	uint8_t col = key_id / 8;
	uint8_t bits = 1 << col;
	if (key_id == 0x0F) {
		bits = 0xFE;
	}
	if (down_or_up) {
		keypad_matrix[row] |= bits;
	} else {
		keypad_matrix[row] &= ~bits;
	}

	if (down_or_up) {
		if (slept) {
			if (key_id >= 0x08 && key_id <= 0x0F && key_id != 0x0E) {
				switch (key_id) {
				case 0x08: wake_up_key = 0x00; break;
				case 0x09: wake_up_key = 0x0A; break;
				case 0x0A: wake_up_key = 0x08; break;
				case 0x0B: wake_up_key = 0x06; break;
				case 0x0C: wake_up_key = 0x04; break;
				case 0x0D: wake_up_key = 0x02; break;
				case 0x0E: wake_up_key = 0x0C; break;
				case 0x0F: wake_up_key = 0x00; break;
				}
				should_wake_up = true;
				wake_up_pending = true;
				slept = false;
			}
		} else {
			if (key_id == 0x0F) {
				slept = true;
			}
		}
	}
}

void ReleaseAllKeys() {
    memset(keypad_matrix, 0, 8);
}

bool CopyLcdBuffer(uint8_t* buffer){
	if (lcd_addr == 0) return false;
	memcpy(buffer, ram_buff + lcd_addr, 1600);
	return true;
}

void RunTimeSlice(uint32_t time_slice, bool speed_up) {
	uint32_t end_cycles = time_slice * cycles_ms;
	register uint32_t cycles = wqx::cycles;
	register uint16_t reg_pc = wqx::reg_pc;
	register uint8_t reg_a = wqx::reg_a;
	register uint8_t reg_ps = wqx::reg_ps;
	register uint8_t reg_x = wqx::reg_x;
	register uint8_t reg_y = wqx::reg_y;
	register uint8_t reg_sp = wqx::reg_sp;

	while (cycles < end_cycles) {
//#ifdef DEBUG
//		if (executed_insts == 2792170) {
//			printf("debug start!\n");
//		}
//		if (executed_insts >= debug_logs.insts_start &&
//			executed_insts < debug_logs.insts_start + debug_logs.insts_count) {
//			log_rec_t& log = debug_logs.logs[executed_insts - debug_logs.insts_start];
//			string debug_info;
//			if (log.reg_pc != reg_pc) {
//				debug_info += " pc ";
//			}
//			if (log.reg_a != reg_a) {
//				debug_info += " a ";
//			}
//			if (log.reg_ps != reg_ps) {
//				debug_info += " ps ";
//			}
//			if (log.reg_x != reg_x) {
//				debug_info += " x ";
//			}
//			if (log.reg_y != reg_y) {
//				debug_info += " y ";
//			}
//			if (log.reg_sp != reg_sp) {
//				debug_info += " sp ";
//			}
//			if (debug_logs.peek_addr != -1) {
//				if (log.peeked != Peek((uint16_t)debug_logs.peek_addr)) {
//					debug_info += " mem ";
//				}
//			} else {
//				if (log.peeked != Peek(reg_pc)) {
//					debug_info += " op ";
//				}
//			}
//			if (debug_info.length()) {
//				printf("%d: %s\n", executed_insts, debug_info.c_str());
//				exit(-1);
//			}
//		}
//		if (executed_insts >= debug_logs.insts_start + debug_logs.insts_count) {
//			printf("ok\n");
//		}
//#endif
		switch (Peek(reg_pc++)) {
		case 0x00: {
			reg_pc++;
			stack[reg_sp--] = reg_pc >> 8;
			stack[reg_sp--] = reg_pc & 0xFF;
			reg_ps |= 0x10;
			stack[reg_sp--] = reg_ps;
			reg_ps |= 0x04;
			reg_pc = PeekW(IRQ_VEC);
			cycles += 7;
		}
			break;
		case 0x01: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 6;
		}
			break;
		case 0x02: {
		}
			break;
		case 0x03: {
		}
			break;
		case 0x04: {
		}
			break;
		case 0x05: {
			uint16_t addr = Peek(reg_pc++);
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 3;
		}
			break;
		case 0x06: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 >> 7);
			tmp1 <<= 1;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 5;
		}
			break;
		case 0x07: {
		}
			break;
		case 0x08: {
			stack[reg_sp--] = reg_ps;
			cycles += 3;
		}
			break;
		case 0x09: {
			uint16_t addr = reg_pc++;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x0A: {
			reg_ps &= 0x7C;
			reg_ps |= reg_a >> 7;
			reg_a <<= 1;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x0B: {
		}
			break;
		case 0x0C: {
		}
			break;
		case 0x0D: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x0E: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 >> 7);
			tmp1 <<= 1;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x0F: {
		}
			break;
		case 0x10: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if (!(reg_ps & 0x80)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0x11: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 5;
		}
			break;
		case 0x12: {
		}
			break;
		case 0x13: {
		}
			break;
		case 0x14: {
		}
			break;
		case 0x15: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x16: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 >> 7);
			tmp1 <<= 1;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x17: {
		}
			break;
		case 0x18: {
			reg_ps &= 0xFE;
			cycles += 2;
		}
			break;
		case 0x19: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x1A: {
		}
			break;
		case 0x1B: {
		}
			break;
		case 0x1C: {
		}
			break;
		case 0x1D: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			reg_a |= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x1E: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 >> 7);
			tmp1 <<= 1;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x1F: {
		}
			break;
		case 0x20: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_pc--;
			stack[reg_sp--] = reg_pc >> 8;
			stack[reg_sp--] = reg_pc & 0xFF;
			reg_pc = addr;
			cycles += 6;
		}
			break;
		case 0x21: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 6;
		}
			break;
		case 0x22: {
		}
			break;
		case 0x23: {
		}
			break;
		case 0x24: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x3D;
			reg_ps |= (!(reg_a & tmp1) << 1) | (tmp1 & 0xC0);
			cycles += 3;
		}
			break;
		case 0x25: {
			uint16_t addr = Peek(reg_pc++);
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 3;
		}
			break;
		case 0x26: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 << 1) | (reg_ps & 0x01);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >> 7);
			Store(addr, tmp2);
			cycles += 5;
		}
			break;
		case 0x27: {
		}
			break;
		case 0x28: {
			reg_ps = stack[++reg_sp];
			cycles += 4;
		}
			break;
		case 0x29: {
			uint16_t addr = reg_pc++;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x2A: {
			uint8_t tmp1 = reg_a;
			reg_a = (reg_a << 1) | (reg_ps & 0x01);
			reg_ps &= 0x7C;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1) | (tmp1 >> 7);
			cycles += 2;
		}
			break;
		case 0x2B: {
		}
			break;
		case 0x2C: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x3D;
			reg_ps |= (!(reg_a & tmp1) << 1) | (tmp1 & 0xC0);
			cycles += 4;
		}
			break;
		case 0x2D: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x2E: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 << 1) | (reg_ps & 0x01);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >> 7);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x2F: {
		}
			break;
		case 0x30: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if ((reg_ps & 0x80)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0x31: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 5;
		}
			break;
		case 0x32: {
		}
			break;
		case 0x33: {
		}
			break;
		case 0x34: {
		}
			break;
		case 0x35: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x36: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 << 1) | (reg_ps & 0x01);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >> 7);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x37: {
		}
			break;
		case 0x38: {
			reg_ps |= 0x01;
			cycles += 2;
		}
			break;
		case 0x39: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x3A: {
		}
			break;
		case 0x3B: {
		}
			break;
		case 0x3C: {
		}
			break;
		case 0x3D: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			reg_a &= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x3E: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 << 1) | (reg_ps & 0x01);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >> 7);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x3F: {
		}
			break;
		case 0x40: {
			reg_ps = stack[++reg_sp];
			reg_pc = stack[++reg_sp];
			reg_pc |= stack[++reg_sp] << 8;
			cycles += 6;
		}
			break;
		case 0x41: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 6;
		}
			break;
		case 0x42: {
		}
			break;
		case 0x43: {
		}
			break;
		case 0x44: {
		}
			break;
		case 0x45: {
			uint16_t addr = Peek(reg_pc++);
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 3;
		}
			break;
		case 0x46: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 & 0x01);
			tmp1 >>= 1;
			reg_ps |= (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 5;
		}
			break;
		case 0x47: {
		}
			break;
		case 0x48: {
			stack[reg_sp--] = reg_a;
			cycles += 3;
		}
			break;
		case 0x49: {
			uint16_t addr = reg_pc++;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x4A: {
			reg_ps &= 0x7C;
			reg_ps |= reg_a & 0x01;
			reg_a >>= 1;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x4B: {
		}
			break;
		case 0x4C: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_pc = addr;
			cycles += 3;
		}
			break;
		case 0x4D: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x4E: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 & 0x01);
			tmp1 >>= 1;
			reg_ps |= (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x4F: {
		}
			break;
		case 0x50: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if (!(reg_ps & 0x40)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0x51: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 5;
		}
			break;
		case 0x52: {
		}
			break;
		case 0x53: {
		}
			break;
		case 0x54: {
		}
			break;
		case 0x55: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x56: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 & 0x01);
			tmp1 >>= 1;
			reg_ps |= (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x57: {
		}
			break;
		case 0x58: {
			reg_ps &= 0xFB;
			cycles += 2;
		}
			break;
		case 0x59: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x5A: {
		}
			break;
		case 0x5B: {
		}
			break;
		case 0x5C: {
		}
			break;
		case 0x5D: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			reg_a ^= Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x5E: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			reg_ps &= 0x7C;
			reg_ps |= (tmp1 & 0x01);
			tmp1 >>= 1;
			reg_ps |= (!tmp1 << 1);
			Store(addr, tmp1);
			cycles += 6;
		}
			break;
		case 0x5F: {
		}
			break;
		case 0x60: {
			reg_pc = stack[++reg_sp];
			reg_pc |= (stack[++reg_sp] << 8);
			reg_pc++;
			cycles += 6;
		}
			break;
		case 0x61: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 6;
		}
			break;
		case 0x62: {
		}
			break;
		case 0x63: {
		}
			break;
		case 0x64: {
		}
			break;
		case 0x65: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 3;
		}
			break;
		case 0x66: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 >> 1) | ((reg_ps & 0x01) << 7);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 & 0x01);
			Store(addr, tmp2);
			cycles += 5;
		}
			break;
		case 0x67: {
		}
			break;
		case 0x68: {
			reg_a = stack[++reg_sp];
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0x69: {
			uint16_t addr = reg_pc++;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 2;
		}
			break;
		case 0x6A: {
			uint8_t tmp1 = reg_a;
			reg_a = (reg_a >> 1) | ((reg_ps & 0x01) << 7);
			reg_ps &= 0x7C;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1) | (tmp1 & 0x01);
			cycles += 2;
		}
			break;
		case 0x6B: {
		}
			break;
		case 0x6C: {
			uint16_t addr = PeekW(PeekW(reg_pc));
			reg_pc += 2;
			reg_pc = addr;
			cycles += 6;
		}
			break;
		case 0x6D: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0x6E: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 >> 1) | ((reg_ps & 0x01) << 7);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 & 0x01);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x6F: {
		}
			break;
		case 0x70: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if ((reg_ps & 0x40)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0x71: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 5;
		}
			break;
		case 0x72: {
		}
			break;
		case 0x73: {
		}
			break;
		case 0x74: {
		}
			break;
		case 0x75: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0x76: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 >> 1) | ((reg_ps & 0x01) << 7);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 & 0x01);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x77: {
		}
			break;
		case 0x78: {
			reg_ps |= 0x04;
			cycles += 2;
		}
			break;
		case 0x79: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0x7A: {
		}
			break;
		case 0x7B: {
		}
			break;
		case 0x7C: {
		}
			break;
		case 0x7D: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a + tmp1 + (reg_ps & 0x01);
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 > 0xFF)
					| (((reg_a ^ tmp1 ^ 0x80) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0x7E: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			uint8_t tmp2 = (tmp1 >> 1) | ((reg_ps & 0x01) << 7);
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 & 0x01);
			Store(addr, tmp2);
			cycles += 6;
		}
			break;
		case 0x7F: {
		}
			break;
		case 0x80: {
		}
			break;
		case 0x81: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			Store(addr, reg_a);
			cycles += 6;
		}
			break;
		case 0x82: {
		}
			break;
		case 0x83: {
		}
			break;
		case 0x84: {
			uint16_t addr = Peek(reg_pc++);
			Store(addr, reg_y);
			cycles += 3;
		}
			break;
		case 0x85: {
			uint16_t addr = Peek(reg_pc++);
			Store(addr, reg_a);
			cycles += 3;
		}
			break;
		case 0x86: {
			uint16_t addr = Peek(reg_pc++);
			Store(addr, reg_x);
			cycles += 3;
		}
			break;
		case 0x87: {
		}
			break;
		case 0x88: {
			reg_y--;
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 2;
		}
			break;
		case 0x89: {
		}
			break;
		case 0x8A: {
			reg_a = reg_x;
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x8B: {
		}
			break;
		case 0x8C: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			Store(addr, reg_y);
			cycles += 4;
		}
			break;
		case 0x8D: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			Store(addr, reg_a);
			cycles += 4;
		}
			break;
		case 0x8E: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			Store(addr, reg_x);
			cycles += 4;
		}
			break;
		case 0x8F: {
		}
			break;
		case 0x90: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if (!(reg_ps & 0x01)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0x91: {
			uint16_t addr = PeekW(Peek(reg_pc));
			addr += reg_y;
			reg_pc++;
			Store(addr, reg_a);
			cycles += 6;
		}
			break;
		case 0x92: {
		}
			break;
		case 0x93: {
		}
			break;
		case 0x94: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			Store(addr, reg_y);
			cycles += 4;
		}
			break;
		case 0x95: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			Store(addr, reg_a);
			cycles += 4;
		}
			break;
		case 0x96: {
			uint16_t addr = (Peek(reg_pc++) + reg_y) & 0xFF;
			Store(addr, reg_x);
			cycles += 4;
		}
			break;
		case 0x97: {
		}
			break;
		case 0x98: {
			reg_a = reg_y;
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0x99: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_y;
			reg_pc += 2;
			Store(addr, reg_a);
			cycles += 5;
		}
			break;
		case 0x9A: {
			reg_sp = reg_x;
			cycles += 2;
		}
			break;
		case 0x9B: {
		}
			break;
		case 0x9C: {
		}
			break;
		case 0x9D: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			Store(addr, reg_a);
			cycles += 5;
		}
			break;
		case 0x9E: {
		}
			break;
		case 0x9F: {
		}
			break;
		case 0xA0: {
			uint16_t addr = reg_pc++;
			reg_y = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 2;
		}
			break;
		case 0xA1: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 6;
		}
			break;
		case 0xA2: {
			uint16_t addr = reg_pc++;
			reg_x = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 2;
		}
			break;
		case 0xA3: {
		}
			break;
		case 0xA4: {
			uint16_t addr = Peek(reg_pc++);
			reg_y = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 3;
		}
			break;
		case 0xA5: {
			uint16_t addr = Peek(reg_pc++);
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 3;
		}
			break;
		case 0xA6: {
			uint16_t addr = Peek(reg_pc++);
			reg_x = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 3;
		}
			break;
		case 0xA7: {
		}
			break;
		case 0xA8: {
			reg_y = reg_a;
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0xA9: {
			uint16_t addr = reg_pc++;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0xAA: {
			reg_x = reg_a;
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 2;
		}
			break;
		case 0xAB: {
		}
			break;
		case 0xAC: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_y = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 4;
		}
			break;
		case 0xAD: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0xAE: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			reg_x = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 4;
		}
			break;
		case 0xAF: {
		}
			break;
		case 0xB0: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if ((reg_ps & 0x01)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0xB1: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 5;
		}
			break;
		case 0xB2: {
		}
			break;
		case 0xB3: {
		}
			break;
		case 0xB4: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			reg_y = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 4;
		}
			break;
		case 0xB5: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0xB6: {
			uint16_t addr = (Peek(reg_pc++) + reg_y) & 0xFF;
			reg_x = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 4;
		}
			break;
		case 0xB7: {
		}
			break;
		case 0xB8: {
			reg_ps &= 0xBF;
			cycles += 2;
		}
			break;
		case 0xB9: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0xBA: {
			reg_x = reg_sp;
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 2;
		}
			break;
		case 0xBB: {
		}
			break;
		case 0xBC: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			reg_y = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 4;
		}
			break;
		case 0xBD: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			reg_a = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_a & 0x80) | (!reg_a << 1);
			cycles += 4;
		}
			break;
		case 0xBE: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			reg_x = Load(addr);
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 4;
		}
			break;
		case 0xBF: {
		}
			break;
		case 0xC0: {
			uint16_t addr = reg_pc++;
			int16_t tmp1 = reg_y - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 2;
		}
			break;
		case 0xC1: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 6;
		}
			break;
		case 0xC2: {
		}
			break;
		case 0xC3: {
		}
			break;
		case 0xC4: {
			uint16_t addr = Peek(reg_pc++);
			int16_t tmp1 = reg_y - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 3;
		}
			break;
		case 0xC5: {
			uint16_t addr = Peek(reg_pc++);
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 3;
		}
			break;
		case 0xC6: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr) - 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 5;
		}
			break;
		case 0xC7: {
		}
			break;
		case 0xC8: {
			reg_y++;
			reg_ps &= 0x7D;
			reg_ps |= (reg_y & 0x80) | (!reg_y << 1);
			cycles += 2;
		}
			break;
		case 0xC9: {
			uint16_t addr = reg_pc++;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 2;
		}
			break;
		case 0xCA: {
			reg_x--;
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 2;
		}
			break;
		case 0xCB: {
		}
			break;
		case 0xCC: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			int16_t tmp1 = reg_y - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xCD: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xCE: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr) - 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xCF: {
		}
			break;
		case 0xD0: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if (!(reg_ps & 0x02)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0xD1: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 5;
		}
			break;
		case 0xD2: {
		}
			break;
		case 0xD3: {
		}
			break;
		case 0xD4: {
		}
			break;
		case 0xD5: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xD6: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr) - 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xD7: {
		}
			break;
		case 0xD8: {
			reg_ps &= 0xF7;
			cycles += 2;
		}
			break;
		case 0xD9: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xDA: {
		}
			break;
		case 0xDB: {
		}
			break;
		case 0xDC: {
		}
			break;
		case 0xDD: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			int16_t tmp1 = reg_a - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xDE: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr) - 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xDF: {
		}
			break;
		case 0xE0: {
			uint16_t addr = reg_pc++;
			int16_t tmp1 = reg_x - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 2;
		}
			break;
		case 0xE1: {
			uint16_t addr = PeekW((Peek(reg_pc++) + reg_x) & 0xFF);
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 6;
		}
			break;
		case 0xE2: {
		}
			break;
		case 0xE3: {
		}
			break;
		case 0xE4: {
			uint16_t addr = Peek(reg_pc++);
			int16_t tmp1 = reg_x - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 3;
		}
			break;
		case 0xE5: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 3;
		}
			break;
		case 0xE6: {
			uint16_t addr = Peek(reg_pc++);
			uint8_t tmp1 = Load(addr) + 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 5;
		}
			break;
		case 0xE7: {
		}
			break;
		case 0xE8: {
			reg_x++;
			reg_ps &= 0x7D;
			reg_ps |= (reg_x & 0x80) | (!reg_x << 1);
			cycles += 2;
		}
			break;
		case 0xE9: {
			uint16_t addr = reg_pc++;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 2;
		}
			break;
		case 0xEA: {
			cycles += 2;
		}
			break;
		case 0xEB: {
		}
			break;
		case 0xEC: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			int16_t tmp1 = reg_x - Load(addr);
			uint8_t tmp2 = tmp1 & 0xFF;
			reg_ps &= 0x7C;
			reg_ps |= (tmp2 & 0x80) | (!tmp2 << 1) | (tmp1 >= 0);
			cycles += 4;
		}
			break;
		case 0xED: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0xEE: {
			uint16_t addr = PeekW(reg_pc);
			reg_pc += 2;
			uint8_t tmp1 = Load(addr) + 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xEF: {
		}
			break;
		case 0xF0: {
			int8_t tmp4 = (int8_t) (Peek(reg_pc++));
			uint16_t addr = reg_pc + tmp4;
			if ((reg_ps & 0x02)) {
				cycles += !((reg_pc ^ addr) & 0xFF00) << 1;
				reg_pc = addr;
			}
			cycles += 2;
		}
			break;
		case 0xF1: {
			uint16_t addr = PeekW(Peek(reg_pc));
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc++;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 5;
		}
			break;
		case 0xF2: {
		}
			break;
		case 0xF3: {
		}
			break;
		case 0xF4: {
		}
			break;
		case 0xF5: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0xF6: {
			uint16_t addr = (Peek(reg_pc++) + reg_x) & 0xFF;
			uint8_t tmp1 = Load(addr) + 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xF7: {
		}
			break;
		case 0xF8: {
			reg_ps |= 0x08;
			cycles += 2;
		}
			break;
		case 0xF9: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_y) & 0xFF00);
			addr += reg_y;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0xFA: {
		}
			break;
		case 0xFB: {
		}
			break;
		case 0xFC: {
		}
			break;
		case 0xFD: {
			uint16_t addr = PeekW(reg_pc);
			cycles += !!(((addr & 0xFF) + reg_x) & 0xFF00);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr);
			int16_t tmp2 = reg_a - tmp1 + (reg_ps & 0x01) - 1;
			uint8_t tmp3 = tmp2 & 0xFF;
			reg_ps &= 0x3C;
			reg_ps |= (tmp3 & 0x80) | (!tmp3 << 1) | (tmp2 >= 0)
					| (((reg_a ^ tmp1) & (reg_a ^ tmp3) & 0x80) >> 1);
			reg_a = tmp3;
			cycles += 4;
		}
			break;
		case 0xFE: {
			uint16_t addr = PeekW(reg_pc);
			addr += reg_x;
			reg_pc += 2;
			uint8_t tmp1 = Load(addr) + 1;
			Store(addr, tmp1);
			reg_ps &= 0x7D;
			reg_ps |= (tmp1 & 0x80) | (!tmp1 << 1);
			cycles += 6;
		}
			break;
		case 0xFF: {
		}
			break;
		}
//#ifdef DEBUG
//		if (should_irq && !(reg_ps & 0x04)) {
//			should_irq = false;
//			stack[reg_sp --] = reg_pc >> 8;
//			stack[reg_sp --] = reg_pc & 0xFF;
//			reg_ps &= 0xEF;
//			stack[reg_sp --] = reg_ps;
//			reg_pc = PeekW(IRQ_VEC);
//			reg_ps |= 0x04;
//			cycles += 7;
//		}
//		executed_insts ++;
//		if (executed_insts % 6000 == 0) {
//			timer1_cycles += CYCLES_TIMER1;
//			clock_buff[4] ++;
//			if (should_wake_up) {
//				should_wake_up = false;
//				ram_io[0x01] |= 0x01;
//				ram_io[0x02] |= 0x01;
//				reg_pc = PeekW(RESET_VEC);
//			} else {
//				ram_io[0x01] |= 0x08;
//				should_irq = true;
//			}
//		}
//#else
		if (cycles >= timer0_cycles) {
			timer0_cycles += cycles_timer0;
			timer0_toggle = !timer0_toggle;
			if (!timer0_toggle) {
				AdjustTime();
			}
			if (!IsCountDown() || timer0_toggle) {
				ram_io[0x3D] = 0;
			} else {
				ram_io[0x3D] = 0x20;
				clock_flags &= 0xFD;
			}
			should_irq = true;
		}
		if (should_irq && !(reg_ps & 0x04)) {
			should_irq = false;
			stack[reg_sp --] = reg_pc >> 8;
			stack[reg_sp --] = reg_pc & 0xFF;
			reg_ps &= 0xEF;
			stack[reg_sp --] = reg_ps;
			reg_pc = PeekW(IRQ_VEC);
			reg_ps |= 0x04;
			cycles += 7;
		}
		if (cycles >= timer1_cycles) {
			if (speed_up) {
				timer1_cycles += cycles_timer1_speed_up;
			} else {
				timer1_cycles += cycles_timer1;
			}
			clock_buff[4] ++;
			if (should_wake_up) {
				should_wake_up = false;
				ram_io[0x01] |= 0x01;
				ram_io[0x02] |= 0x01;
				reg_pc = PeekW(RESET_VEC);
			} else {
				ram_io[0x01] |= 0x08;
				should_irq = true;
			}
		}
//#endif
	}

	cycles -= end_cycles;
	timer0_cycles = (end_cycles > timer0_cycles) ? 0 : (timer0_cycles - end_cycles);
	timer1_cycles = (end_cycles > timer1_cycles) ? 0 : (timer1_cycles - end_cycles);

	wqx::reg_pc = reg_pc;
	wqx::reg_a = reg_a;
	wqx::reg_ps = reg_ps;
	wqx::reg_x = reg_x;
	wqx::reg_y = reg_y;
	wqx::reg_sp = reg_sp;
}

}
