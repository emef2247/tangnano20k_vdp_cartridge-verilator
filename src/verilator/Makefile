# Makefile: Verilatorビルド

# RTL/ダミーモジュールディレクトリ
VPATH = /home/emef220/projects/TangCartMSX/RTL/tangnano20k_vdp_cartridge_rev2_step1/src
MOD   = ./mod
RTL   = ./RTL

# Verilator共通オプション
VERILATOR_FLAGS = -Wno-fatal -I$(VPATH)

# RTLファイルリスト（mod優先で頭に並べる！）
# wrapper_top.sv をトップに追加し、その下に DUT 一式を列挙
VERILOG_SRCS = \
	$(RTL)/wrapper_top.sv \
	$(MOD)/ip_sdram_tangnano20k_c.v \
	$(MOD)/ip_sdram.v \
	$(VPATH)/test_top_SCREEN5_SP2/dummy.v \
	$(VPATH)/msx_slot/msx_slot.v \
	$(VPATH)/th9958/vdp_upscan_line_buffer.v \
	$(VPATH)/th9958/vdp_upscan.v \
	$(VPATH)/th9958/vdp_video_ram_line_buffer.v \
	$(VPATH)/th9958/vdp_video_double_buffer.v \
	$(VPATH)/th9958/vdp_video_out_bilinear.v \
	$(VPATH)/th9958/vdp_video_out.v \
	$(VPATH)/th9958/vdp_sprite_info_collect.v \
	$(VPATH)/th9958/vdp_sprite_makeup_pixel.v \
	$(VPATH)/th9958/vdp_sprite_select_visible_planes.v \
	$(VPATH)/th9958/vdp_sprite_divide_table.v \
	$(VPATH)/th9958/vdp_timing_control_ssg.v \
	$(VPATH)/th9958/vdp_timing_control_screen_mode.v \
	$(VPATH)/th9958/vdp_timing_control_sprite.v \
	$(VPATH)/th9958/vdp_timing_control.v \
	$(VPATH)/th9958/vdp_vram_interface.v \
	$(VPATH)/th9958/vdp_cpu_interface.v \
	$(VPATH)/th9958/vdp_color_palette_ram.v \
	$(MOD)/vdp_color_palette.v \
	$(VPATH)/th9958/vdp_command_cache.v \
	$(VPATH)/th9958/vdp_command.v \
	$(VPATH)/th9958/vdp.v \
	$(VPATH)/ws2812_led/ip_ws2812_led.v \
	$(VPATH)/debugger/ip_debugger.v \
	$(VPATH)/tangnano20k_vdp_cartridge.v

# C++ソース
C_SRCS = vdp_cartridge_wrapper.cpp main.cpp

# ----------------
all: sim

# Verilator が生成する実行バイナリ名（top-module = wrapper_top）
SIM_BIN = obj_dir/Vwrapper_top

sim: $(SIM_BIN)
	./$(SIM_BIN)

$(SIM_BIN): $(VERILOG_SRCS) $(C_SRCS)
	verilator --cc --exe --build --trace $(VERILATOR_FLAGS) \
		$(VERILOG_SRCS) $(C_SRCS) --top-module wrapper_top

clean:
	rm -rf obj_dir Vwrapper_top* Vtangnano20k_vdp_cartridge* *.o *.d

.PHONY: all sim clean