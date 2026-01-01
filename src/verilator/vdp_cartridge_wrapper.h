#ifndef VDP_CARTRIDGE_WRAPPER_H
#define VDP_CARTRIDGE_WRAPPER_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void vdp_cartridge_init(void);
void vdp_cartridge_release(void);
void vdp_cartridge_reset(void);
void vdp_cartridge_step_clk_posedge(void);
void vdp_cartridge_step_clk_negedge(void);
static inline void vdp_cartridge_step_clk_1cycle(void) {
    vdp_cartridge_step_clk_posedge();
    vdp_cartridge_step_clk_negedge();
}
void vdp_cartridge_set_button(uint8_t v);
void vdp_cartridge_set_dipsw(uint8_t v);

// バス経由のDRAMアクセス
void vdp_cartridge_dram_write(uint32_t addr, uint32_t data, uint8_t mask);
uint32_t vdp_cartridge_dram_read(uint32_t addr);
void vdp_cartridge_dram_dump(FILE* fp);

// メモリダンプ/getter
void* vdp_cartridge_get_vram_buffer(void);
size_t vdp_cartridge_get_vram_size(void);

// 内部コア用: バス信号監視(eval中自動呼び出す)
void vdp_cartridge_sdram_bus_eval(void);

/* write task emulation */
void vdp_cartridge_write_io(uint16_t address, uint8_t wdata);

/* Mode selector (0=negedge tb mode, 1=posedge alternate) */
void vdp_cartridge_set_write_on_posedge(int enable);

/* Enable runtime debug logging (0/1) */
void vdp_cartridge_set_debug(int enable);

/* Control whether write_io forces end-align (advance final negedge) */
void vdp_cartridge_set_end_align(int enable);

/* VCD トレース制御 */
int  vdp_cartridge_trace_open(const char* path); /* returns 0 on success, -1 on failure */
void vdp_cartridge_trace_close(void);

/* シミュレーション時刻取得 (ps 単位) */
uint64_t vdp_cartridge_get_sim_time(void);

/* slot_wait getter */
uint8_t vdp_cartridge_get_slot_wait(void);

#ifdef __cplusplus
}
#endif

#endif /* VDP_CARTRIDGE_WRAPPER_H */