#include "nes.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t ram[0x800];
uint8_t sram[0x2000];
uint8_t prg[8][0x4000];
uint8_t prg_map[2] = {0, 7};
uint8_t open_bus = 0;

uint8_t A, X, Y, S, P;
uint16_t PC;
int cycles = 0;

int frame_done = 0;
int nmi_pending = 0;
int irq_pending = 0;
int nes_frame = 0;

uint8_t controller_state = 0;
static uint8_t ctrl_strobe = 0;
static uint16_t ctrl_shift = 0xFF00;

// VRC4 state
static uint8_t vrc_prg0 = 0;
static uint8_t vrc_prg1 = 1;
static uint8_t vrc_prg_mode = 0;
static uint8_t vrc_chr_lo[8] = {0};
static uint8_t vrc_chr_hi[8] = {0};

// VRC4 IRQ
static uint8_t vrc_irq_reload = 0;
static uint8_t vrc_irq_counter = 0;
static int vrc_irq_prescaler = 0;
static uint8_t vrc_irq_enabled = 0;
static uint8_t vrc_irq_enabled_after_ack = 0;
static uint8_t vrc_irq_cycle_mode = 0;

void vrc4_step_irq(int cpu_cycles) {
    if (vrc_irq_enabled) {
        for (int c = 0; c < cpu_cycles; c++) {
            vrc_irq_prescaler -= 3;
            if (vrc_irq_cycle_mode || (!vrc_irq_cycle_mode && vrc_irq_prescaler <= 0)) {
                if (vrc_irq_counter == 0xFF) {
                    vrc_irq_counter = vrc_irq_reload;
                    irq_pending = 1;
                } else {
                    vrc_irq_counter++;
                }
                vrc_irq_prescaler += 341;
            }
        }
    }
}

static void vrc4_update_prg(void) {
    prg_map[0] = (uint8_t)((vrc_prg0 >> 1) & 7);
    prg_map[1] = 7;
}

static void vrc4_update_chr(int slot) {
    uint8_t bank = (uint8_t)(vrc_chr_lo[slot] | ((vrc_chr_hi[slot] & 0x1F) << 4));
    ppu_set_chr_bank(slot, bank);
}

void mmc1_write(uint16_t addr, uint8_t v) {
    uint32_t a0 = (addr >> 2) & 1;
    uint32_t a1 = (addr >> 3) & 1;
    uint16_t reg = (addr & 0xF000) | (uint16_t)((a1 << 1) | a0);

    if (reg >= 0x8000 && reg <= 0x8003) {
        vrc_prg0 = v & 0x1F;
        vrc4_update_prg();
    } else if (reg >= 0x9000 && reg <= 0x9001) {
        static const uint8_t mirr_conv[4] = {2, 3, 0, 1};
        ppu_set_mirror(mirr_conv[v & 3]);
    } else if (reg >= 0x9002 && reg <= 0x9003) {
        vrc_prg_mode = (v >> 1) & 1;
        vrc4_update_prg();
    } else if (reg >= 0xA000 && reg <= 0xA003) {
        vrc_prg1 = v & 0x1F;
        vrc4_update_prg();
    } else if (reg >= 0xB000 && reg <= 0xE003) {
        int slot = ((((reg >> 12) & 7) - 3) << 1) + ((reg >> 1) & 1);
        if (slot >= 0 && slot < 8) {
            if ((reg & 1) == 0) {
                vrc_chr_lo[slot] = v & 0x0F;
            } else {
                vrc_chr_hi[slot] = v & 0x1F;
            }
            vrc4_update_chr(slot);
        }
    } else if (reg == 0xF000) {
        vrc_irq_reload = (uint8_t)((vrc_irq_reload & 0xF0) | (v & 0x0F));
    } else if (reg == 0xF001) {
        vrc_irq_reload = (uint8_t)((vrc_irq_reload & 0x0F) | ((v & 0x0F) << 4));
    } else if (reg == 0xF002) {
        vrc_irq_enabled_after_ack = (v & 0x01);
        vrc_irq_enabled = (v & 0x02) ? 1 : 0;
        vrc_irq_cycle_mode = (v & 0x04) ? 1 : 0;
        if (vrc_irq_enabled) {
            vrc_irq_counter = vrc_irq_reload;
            vrc_irq_prescaler = 341;
        }
        irq_pending = 0;
    } else if (reg == 0xF003) {
        vrc_irq_enabled = vrc_irq_enabled_after_ack;
        irq_pending = 0;
    }
}

static void oam_dma(uint8_t page) {
    uint16_t base = (uint16_t)(page << 8);
    for (int i = 0; i < 256; i++)
        oam_write_dma(rd8((uint16_t)(base + i)));
    cycles += 513;
}

uint8_t rd8(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a < 0x4000) { open_bus = ppu_read(a & 7); return open_bus; }
    if (a == 0x4015) { open_bus = apu_read(0x15); return open_bus; }
    if (a == 0x4016) {
        if (trace_chr)
            fprintf(stderr, "PADRD %d:%04X A=%02X X=%02X Y=%02X\n", cur_bank, cur_pc, A, X, Y);
        uint8_t v;
        if (ctrl_strobe) {
            v = (uint8_t)(controller_state & 1);
        } else {
            v = (uint8_t)(ctrl_shift & 1);
            ctrl_shift = (uint16_t)((ctrl_shift >> 1) | 0x8000);
        }
        open_bus = v;
        return v;
    }
    if (a == 0x4017) { open_bus = (uint8_t)(0x40); return open_bus; }
    if (a < 0x4020) return open_bus;
    if (a < 0x6000) return open_bus;
    if (a < 0x8000) return sram[a - 0x6000];
    open_bus = prg_read(a);
    return open_bus;
}

void wr8(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7FF] = v; return; }
    if (a < 0x4000) { ppu_write(a & 7, v); return; }
    if (a == 0x4014) { oam_dma(v); return; }
    if (a == 0x4016) {
        ctrl_strobe = (uint8_t)(v & 1);
        if (ctrl_strobe) ctrl_shift = (uint16_t)(0xFF00 | controller_state);
        return;
    }
    if (a <= 0x4017) { apu_write(a, v); return; }
    if (a < 0x4020) return;
    if (a < 0x6000) return;
    if (a < 0x8000) { sram[a - 0x6000] = v; return; }
    mmc1_write(a, v);
}

void do_rti(void) {
    P = (uint8_t)((pop8() & ~0x30) | 0x20);
    uint16_t lo = pop8();
    uint16_t hi = pop8();
    PC = (uint16_t)(lo | (hi << 8));
}

void do_brk(uint16_t next) {
    fprintf(stderr, "BRK at next=%04X\n", next);
    dump_trace();
    push16(next);
    push8((uint8_t)(P | 0x30));
    P |= FI;
    PC = kIrqVector;
}

void illegal_op(uint16_t addr) {
    fprintf(stderr, "illegal opcode reached at $%04X (bank %d)\n", addr,
            prg_map[addr >= 0xC000 ? 1 : 0]);
    fprintf(stderr, "A=%02X X=%02X Y=%02X S=%02X P=%02X\n", A, X, Y, S, P);
    exit(2);
}

// --------------------------------------------------------------------------
static void (*block_tbl[16][0x8000])(void);
unsigned long long cpu_blocks = 0;
uint16_t cur_pc = 0;
uint8_t cur_bank = 0;
unsigned long long nmi_taken = 0;
unsigned long long vblank_events = 0;

#define TRACE_N 64
static uint16_t trace_pc[TRACE_N];
static uint8_t trace_bank[TRACE_N];
static unsigned trace_i = 0;

static void trace_push(uint16_t pc, uint8_t bank) {
    extern int trace_chr;
    if (trace_chr && pc >= 0xEA28 && pc <= 0xEA80)
        fprintf(stderr, "EA pc=%04X A=%02X X=%02X Y=%02X z14=%02X%02X z6A=%02X bnk=%02X\n",
                pc, A, X, Y, ram[0x15], ram[0x14], ram[0x6A], prg_map[0]);
    if (trace_chr && pc == 0xE95A)
        fprintf(stderr, "BLIT dest=%04X pages=%d src=%04X bank=%02X\n",
                A << 8, X, ram[0x10] | (ram[0x11] << 8), prg_map[0]);
    trace_pc[trace_i] = pc;
    trace_bank[trace_i] = bank;
    trace_i = (trace_i + 1) % TRACE_N;
}

void dump_trace(void) {
    fprintf(stderr, "--- last PCs ---\n");
    for (int k = 0; k < TRACE_N; k++) {
        unsigned i = (trace_i + k) % TRACE_N;
        fprintf(stderr, "%02d:%02X:%04X  ", k, trace_bank[i], trace_pc[i]);
        if ((k % 4) == 3) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\nA=%02X X=%02X Y=%02X S=%02X P=%02X prg_map=%02X/%02X\n",
            A, X, Y, S, P, prg_map[0], prg_map[1]);
}
static int missing_logged = 0;

void cpu_run_frame(void) {
    frame_done = 0;
    while (!frame_done) {
        if (cycles) {
            ppu_step(cycles);
            apu_step(cycles * 3);
            vrc4_step_irq(cycles);
            cycles = 0;
        }
        if (frame_done) break;

        if (nmi_pending) {
            nmi_pending = 0;
            nmi_taken++;
            push16(PC);
            push8((uint8_t)((P & ~0x10) | 0x20));
            P |= FI;
            PC = kNmiVector;
        } else if (irq_pending && !(P & FI)) {
            push16(PC);
            push8((uint8_t)((P & ~0x10) | 0x20));
            P |= FI;
            PC = kIrqVector;
        }

        int bank = (PC >= 0xC000) ? prg_map[1] : prg_map[0];
        void (*fn)(void) = block_tbl[bank][PC & 0x7FFF];
        if (!fn) {
            if (!missing_logged) {
                fprintf(stderr, "MISSING block bank=%d addr=%04X\n", bank, PC);
                dump_trace();
            }
            missing_logged = 1;
            cpu_fault = 1;
            return;
        }
        {
            static int trapped = 0;
            if (PC == 0xFEBB && !trapped) {
                trapped = 1;
                fprintf(stderr, "TRAP FEBB\n");
                dump_trace();
            }
        }
        cur_pc = PC;
        cur_bank = (uint8_t)bank;
        trace_push(PC, (uint8_t)bank);
        fn();
        cpu_blocks++;
    }
    nes_frame++;
}

void nes_init(void) {
    for (unsigned i = 0; i < kEntryCount; i++)
        block_tbl[kEntries[i].bank][kEntries[i].addr & 0x7FFF] = kEntries[i].fn;

    // NES power-on RAM is undefined; the reference (jsnes) fills it with $FF
    // and the game reads uninitialised bytes (e.g. $6A during class select).
    memset(ram, 0xFF, sizeof(ram));
    A = X = Y = 0;
    S = 0xFD;
    P = 0x24;
    PC = kResetVector;
    cycles = 0;
    vrc_prg0 = 0;
    vrc_prg1 = 1;
    vrc_prg_mode = 0;
    vrc4_update_prg();
    vrc_irq_reload = 0;
    vrc_irq_counter = 0;
    vrc_irq_prescaler = 0;
    vrc_irq_enabled = 0;
    vrc_irq_enabled_after_ack = 0;
    vrc_irq_cycle_mode = 0;
    irq_pending = 0;
    ppu_reset();
    apu_reset();
}

void mem_state(StateArchive &a) {
    a.value(A); a.value(X); a.value(Y); a.value(S); a.value(P); a.value(PC); a.value(cycles);
    a.bytes(ram,sizeof(ram)); a.bytes(sram,sizeof(sram));
    a.bytes(prg_map,sizeof(prg_map)); a.value(open_bus);
    a.value(frame_done); a.value(nmi_pending); a.value(nes_frame);
    a.value(controller_state); a.value(ctrl_strobe); a.value(ctrl_shift);
    a.value(vrc_prg0); a.value(vrc_prg1); a.value(vrc_prg_mode);
    a.bytes(vrc_chr_lo, sizeof(vrc_chr_lo)); a.bytes(vrc_chr_hi, sizeof(vrc_chr_hi));
    a.value(vrc_irq_reload); a.value(vrc_irq_counter); a.value(vrc_irq_prescaler);
    a.value(vrc_irq_enabled); a.value(vrc_irq_enabled_after_ack); a.value(vrc_irq_cycle_mode);
    a.value(irq_pending);
    a.value(cpu_blocks); a.value(cur_pc); a.value(cur_bank); a.value(nmi_taken); a.value(vblank_events);
    a.bytes(trace_pc,sizeof(trace_pc)); a.bytes(trace_bank,sizeof(trace_bank)); a.value(trace_i);
    a.value(missing_logged); a.value(cpu_fault);
}
