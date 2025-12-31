//
// ip_sdram.v
//
//	Copyright (C) 2025 Takayuki Hara
//
//	本ソフトウェアおよび本ソフトウェアに基づいて作成された派生物は、以下の条件を
//	満たす場合に限り、再頒布および使用が許可されます。
//
//	1.ソースコード形式で再頒布する場合、上記の著作権表示、本条件一覧、および下記
//	  免責条項をそのままの形で保持すること。
//	2.バイナリ形式で再頒布する場合、頒布物に付属のドキュメント等の資料に、上記の
//	  著作権表示、本条件一覧、および下記免責条項を含めること。
//	3.書面による事前の許可なしに、本ソフトウェアを販売、および商業的な製品や活動
//	  に使用しないこと。
//
//	本ソフトウェアは、著作権者によって「現状のまま」提供されています。著作権者は、
//	特定目的への適合性の保証、商品性の保証、またそれに限定されない、いかなる明示
//	的もしくは暗黙な保証責任も負いません。著作権者は、事由のいかんを問わず、損害
//	発生の原因いかんを問わず、かつ責任の根拠が契約であるか厳格責任であるか（過失
//	その他の）不法行為であるかを問わず、仮にそのような損害が発生する可能性を知ら
//	されていたとしても、本ソフトウェアの使用によって発生した（代替品または代用サ
//	ービスの調達、使用の喪失、データの喪失、利益の喪失、業務の中断も含め、またそ
//	れに限定されない）直接損害、間接損害、偶発的な損害、特別損害、懲罰的損害、ま
//	たは結果損害について、一切責任を負わないものとします。
//
//	Note that above Japanese version license is the formal document.
//	The following translation is only for reference.
//
//	Redistribution and use of this software or any derivative works,
//	are permitted provided that the following conditions are met:
//
//	1. Redistributions of source code must retain the above copyright
//	   notice, this list of conditions and the following disclaimer.
//	2. Redistributions in binary form must reproduce the above
//	   copyright notice, this list of conditions and the following
//	   disclaimer in the documentation and/or other materials
//	   provided with the distribution.
//	3. Redistributions may not be sold, nor may they be used in a
//	   commercial product or activity without specific prior written
//	   permission.
//
//	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//	"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//	LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
//	FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//	COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
//	BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//	LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//	LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
//	ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//	POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------

// [Optional] Modifications by @emef2247 2025-12-29: Verilator対応のため3値ロジック削除等


module ip_sdram_tangnano20k_c #(
	parameter		FREQ = 85_909_080
) (
	input				reset_n,
	input				clk,
	input				clk_sdram,
	output				sdram_init_busy,

	input	[22:2]		bus_address,
	input				bus_valid,
	input				bus_write,
	input				bus_refresh,
	input	[31:0]		bus_wdata,
	input	[3:0]		bus_wdata_mask,
	output	[31:0]		bus_rdata,
	output				bus_rdata_en,

	output				O_sdram_clk,
	output				O_sdram_cke,
	output				O_sdram_cs_n,
	output				O_sdram_ras_n,
	output				O_sdram_cas_n,
	output				O_sdram_wen_n,
	inout	[31:0]		IO_sdram_dq,
	output	[10:0]		O_sdram_addr,
	output	[ 1:0]		O_sdram_ba,
	output	[ 3:0]		O_sdram_dqm
);

	localparam	[3:0]	c_sdr_command_mode_register_set		= 4'b0000;
	localparam	[3:0]	c_sdr_command_refresh				= 4'b0001;
	localparam	[3:0]	c_sdr_command_precharge_all			= 4'b0010;
	localparam	[3:0]	c_sdr_command_activate				= 4'b0011;
	localparam	[3:0]	c_sdr_command_write					= 4'b0100;
	localparam	[3:0]	c_sdr_command_read					= 4'b0101;
	localparam	[3:0]	c_sdr_command_burst_stop			= 4'b0110;
	localparam	[3:0]	c_sdr_command_no_operation			= 4'b0111;
	localparam	[3:0]	c_sdr_command_deselect				= 4'b1111;

	localparam	[4:0]	c_init_state_begin_first_wait		= 5'd0;
	localparam	[4:0]	c_init_state_first_wait				= 5'd1;
	localparam	[4:0]	c_init_state_send_precharge_all		= 5'd2;
	localparam	[4:0]	c_init_state_wait_precharge_all		= 5'd3;
	localparam	[4:0]	c_init_state_send_refresh_all1		= 5'd4;
	localparam	[4:0]	c_init_state_wait_refresh_all1		= 5'd5;
	localparam	[4:0]	c_init_state_send_refresh_all2		= 5'd6;
	localparam	[4:0]	c_init_state_wait_refresh_all2		= 5'd7;
	localparam	[4:0]	c_init_state_send_mode_register_set	= 5'd8;
	localparam	[4:0]	c_init_state_wait_mode_register_set	= 5'd9;
	localparam	[4:0]	c_main_state_ready					= 5'd10;
	localparam	[4:0]	c_main_state_activate				= 5'd11;
	localparam	[4:0]	c_main_state_nop1					= 5'd12;
	localparam	[4:0]	c_main_state_nop2					= 5'd13;
	localparam	[4:0]	c_main_state_read_or_write			= 5'd14;
	localparam	[4:0]	c_main_state_nop3					= 5'd15;
	localparam	[4:0]	c_main_state_data_fetch				= 5'd16;
	localparam	[4:0]	c_main_state_finish					= 5'd17;
	localparam	[4:0]	c_main_state_nop4					= 5'd18;
	localparam	[4:0]	c_main_state_nop5					= 5'd19;
	localparam	[4:0]	c_main_state_nop6					= 5'd20;
	localparam	[4:0]	c_main_state_nop7					= 5'd21;
	localparam	[4:0]	c_main_state_finish2				= 5'd22;

	localparam CLOCK_TIME		= 1_000_000_000 / FREQ;
	localparam TIMER_COUNT		= 300_000 / CLOCK_TIME;
	localparam TIMER_BITS		= $clog2(TIMER_COUNT + 1);
	localparam REFRESH_COUNT	= 15_000 / CLOCK_TIME;
	localparam REFRESH_BITS		= $clog2(REFRESH_COUNT + 1);
	localparam REFRESH_NONE		= 10_000 / CLOCK_TIME;

	reg		[ 4:0]				ff_main_state;
	reg		[TIMER_BITS-1:0]	ff_main_timer;
	wire						w_end_of_main_timer;

	reg							ff_sdr_ready;
	reg							ff_do_main_state;
	reg							ff_do_refresh;

	reg		[ 3:0]				ff_sdr_command;
	reg		[ 1:0]				ff_sdr_bank;
	reg		[10:0]				ff_sdr_address;
	reg		[31:0]				ff_sdr_write_data;
	reg		[ 3:0]				ff_sdr_dq_mask;
	reg		[31:0]				ff_sdr_read_data;
	reg							ff_sdr_read_data_en;
	reg							ff_do_command;
	reg							ff_write;
	reg		[31:0]				ff_wdata;
	reg		[ 3:0]				ff_wdata_mask;
	reg		[ 1:0]				ff_bank;
	reg		[10:0]				ff_row_address;
	reg		[ 7:0]				ff_col_address;
	wire						w_busy;
	reg							ff_initial_finish;

	// Request latch
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_do_command	<= 1'b0;
			ff_write		<= 1'b0;
			ff_wdata		<= 32'd0;
			ff_wdata_mask	<= 4'd0;
			ff_bank			<= 2'd0;
			ff_row_address	<= 11'd0;
			ff_col_address	<= 8'd0;
			ff_do_refresh	<= 1'b0;
		end
		else if( (ff_main_state == c_main_state_ready) && (bus_valid || bus_refresh) ) begin
			ff_do_command	<= 1'b1;
			ff_do_refresh	<= bus_refresh;
			ff_write		<= bus_write;
			ff_wdata		<= bus_wdata;
			ff_wdata_mask	<= bus_wdata_mask;
			ff_bank			<= bus_address[22:21];
			ff_row_address	<= bus_address[20:10];
			ff_col_address	<= bus_address[9:2];
		end
		else if( (ff_main_state == c_main_state_finish) || (ff_main_state == c_main_state_finish2) ) begin
			ff_do_command	<= 1'b0;
			ff_do_refresh	<= 1'b0;
			ff_write		<= 1'b0;
		end
	end

	// Main State Machine
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_main_state		<= c_init_state_begin_first_wait;
			ff_do_main_state	<= 1'b0;
		end
		else begin
			case( ff_main_state )
			c_init_state_begin_first_wait:
				ff_main_state	<= c_init_state_first_wait;
			c_init_state_send_precharge_all:
				ff_main_state	<= c_init_state_wait_precharge_all;
			c_init_state_send_refresh_all1:
				ff_main_state	<= c_init_state_wait_refresh_all1;
			c_init_state_send_refresh_all2:
				ff_main_state	<= c_init_state_wait_refresh_all2;
			c_init_state_send_mode_register_set:
				ff_main_state	<= c_init_state_wait_mode_register_set;
			c_main_state_ready:
				if( bus_valid || bus_refresh ) begin
					ff_main_state		<= c_main_state_activate;
				end
			c_main_state_activate:
				begin
					ff_main_state		<= c_main_state_nop1;
					ff_do_main_state	<= 1'b1;
				end
			c_main_state_read_or_write:
				begin
					if( ff_do_refresh ) begin
						ff_main_state		<= c_main_state_nop4;
					end
					else begin
						ff_main_state		<= c_main_state_nop3;
					end
				end
			c_main_state_finish, c_main_state_finish2:
				begin
					ff_main_state		<= c_main_state_ready;
					ff_do_main_state	<= 1'b0;
				end
			default:
				if( (!ff_sdr_ready && w_end_of_main_timer) || (ff_sdr_ready && ff_do_main_state) ) begin
					ff_main_state	<= ff_main_state + 5'd1;
				end
			endcase
		end
	end

	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_initial_finish	<= 1'b0;
		end
		else if( w_end_of_main_timer && ff_main_state == c_init_state_first_wait ) begin
			ff_initial_finish	<= 1'b1;
		end
	end

	assign w_busy			= ff_do_command;
	assign sdram_init_busy	= !ff_sdr_ready;

	// Sub-state (SDRAM ready)
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_sdr_ready	<= 1'b0;
		end
		else if( (ff_main_state == c_init_state_wait_mode_register_set) && w_end_of_main_timer ) begin
			ff_sdr_ready	<= 1'b1;
		end
	end

	// Main timer
	always @( posedge clk ) begin
		case( ff_main_state )
		c_init_state_begin_first_wait:
			ff_main_timer	<= TIMER_COUNT;
		c_init_state_send_precharge_all:
			ff_main_timer	<= 'd5;
		c_init_state_send_refresh_all1:
			ff_main_timer	<= 'd15;
		c_init_state_send_refresh_all2:
			ff_main_timer	<= 'd15;
		c_init_state_send_mode_register_set:
			ff_main_timer	<= 'd2;
		default:
			if( !w_end_of_main_timer ) begin
				ff_main_timer	<= ff_main_timer - 'd1;
			end
		endcase
	end

	assign w_end_of_main_timer	= (ff_main_timer == 'd0) ? 1'b1 : 1'b0;

	// SDRAM Command signal
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_sdr_command		<= c_sdr_command_no_operation;
			ff_sdr_dq_mask		<= 4'b1111;
		end
		else begin
			case( ff_main_state )
			c_init_state_send_precharge_all:
				begin
					ff_sdr_command		<= c_sdr_command_precharge_all;
					ff_sdr_dq_mask		<= 4'b1111;
				end
			c_init_state_send_refresh_all1, c_init_state_send_refresh_all2:
				begin
					ff_sdr_command		<= c_sdr_command_refresh;
					ff_sdr_dq_mask		<= 4'b0000;
				end
			c_init_state_send_mode_register_set:
				begin
					ff_sdr_command		<= c_sdr_command_mode_register_set;
					ff_sdr_dq_mask		<= 4'b1111;
				end
			default:
				if( ff_sdr_ready ) begin
					case( ff_main_state )
					c_main_state_activate:
						if( ff_do_refresh ) begin
							ff_sdr_command		<= c_sdr_command_precharge_all;
							ff_sdr_dq_mask		<= 4'b0000;
						end
						else begin
							ff_sdr_command		<= c_sdr_command_activate;
							ff_sdr_dq_mask		<= 4'b1111;
						end
					c_main_state_read_or_write:
						if( ff_do_refresh ) begin
							ff_sdr_command		<= c_sdr_command_refresh;
							ff_sdr_dq_mask		<= 4'b0000;
						end
						else if( ff_write ) begin
							ff_sdr_command		<= c_sdr_command_write;
							ff_sdr_dq_mask		<= ff_wdata_mask;
						end
						else begin
							ff_sdr_command		<= c_sdr_command_read;
							ff_sdr_dq_mask		<= 4'b0000;
						end
					default:
						begin
							ff_sdr_command		<= c_sdr_command_no_operation;
							ff_sdr_dq_mask		<= 4'b1111;
						end
					endcase
				end
				else begin
					ff_sdr_command		<= c_sdr_command_no_operation;
					ff_sdr_dq_mask		<= 4'b1111;
				end
			endcase
		end
	end

	// SDRAM BANK / ADDRESS
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_sdr_bank <= 2'd0;
			ff_sdr_address <= 11'd0;
		end
		else if( !ff_sdr_ready ) begin
			case( ff_main_state )
			c_init_state_send_precharge_all:
				begin
					ff_sdr_bank <= 2'd0;
					ff_sdr_address <= {1'b1, 10'd0};
				end
			default:
				begin
					ff_sdr_bank <= 2'd0;
					ff_sdr_address <= {1'b0, 1'b1, 2'b00, 3'b010, 1'b0, 3'b000};
				end
			endcase
		end
		else begin
			case( ff_main_state )
			c_main_state_activate:
				begin
					ff_sdr_bank <= ff_bank;
					ff_sdr_address <= ff_row_address;
				end
			c_main_state_read_or_write:
				begin
					if( ff_do_refresh ) begin
						ff_sdr_bank <= 2'd0;
						ff_sdr_address <= {1'b1, 10'd0};
					end
					else begin
						ff_sdr_bank <= ff_bank;
						ff_sdr_address <= {1'b1, 2'd0, ff_col_address};
					end
				end
			default:
				; // hold
			endcase
		end
	end

	// --- Verilator用3値削除: 書込みデータ ---
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_sdr_write_data <= 32'd0; // Z禁止！0で初期化
		end
		else if( ff_main_state == c_main_state_read_or_write ) begin
			ff_sdr_write_data <= ff_wdata;
		end
		else begin
			ff_sdr_write_data <= 32'd0; // アイドル・リセット時は0ドライブ
		end
	end

	// Read path
	always @( posedge clk_sdram ) begin
		if( !reset_n ) begin
			ff_sdr_read_data	<= 32'd0;
		end
		else if( ff_main_state == c_main_state_finish ) begin
			ff_sdr_read_data	<= IO_sdram_dq;
		end
	end

	// Read data enable
	always @( posedge clk ) begin
		if( !reset_n ) begin
			ff_sdr_read_data_en	<= 1'b0;
		end
		else if( ff_main_state == c_main_state_finish ) begin
			ff_sdr_read_data_en	<= ~ff_write & ~ff_do_refresh;
		end
		else if( ff_sdr_read_data_en ) begin
			ff_sdr_read_data_en	<= 1'b0;
		end
	end

	// SDRAM I/F
	assign O_sdram_clk		= clk_sdram;
	assign O_sdram_cke		= ff_initial_finish;
	assign O_sdram_cs_n		= ff_sdr_command[3];
	assign O_sdram_ras_n	= ff_sdr_command[2];
	assign O_sdram_cas_n	= ff_sdr_command[1];
	assign O_sdram_wen_n	= ff_sdr_command[0];

	assign O_sdram_dqm		= ff_sdr_dq_mask;
	assign O_sdram_ba		= ff_sdr_bank;
	assign O_sdram_addr		= ff_sdr_address;

	// --- Verilator対応: inoutは常に出力駆動 ---
	assign IO_sdram_dq		= ff_sdr_write_data;

	assign bus_rdata		= ff_sdr_read_data;
	assign bus_rdata_en		= ff_sdr_read_data_en;

endmodule
