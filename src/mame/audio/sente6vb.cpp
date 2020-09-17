// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    Bally/Sente 6VB audio board emulation

    This serial audio board appears to be based on the Sequential Circuits
    Six-Trak synthesizer.

    The later revision of this board replaces the 8253-5 PIT and much
    associated logic with a ST1001 custom gate array.

****************************************************************************

    Memory map

****************************************************************************

    ========================================================================
    Z80 CPU
    ========================================================================
    0000-1FFF   R     xxxxxxxx    Program ROM
    2000-3FFF   R/W   xxxxxxxx    Option RAM/ROM (assumed to be RAM for now)
    4000-5FFF   R/W   xxxxxxxx    Program RAM
    6000-6001     W   xxxxxxxx    6850 UART output (to main board)
    E000-E001   R     xxxxxxxx    6850 UART input (from main board)
    ========================================================================
    0000-0003   R/W   xxxxxxxx    8253 counter chip I/O
    0008        R     ------xx    Counter state
                R     ------x-    State of counter #0 OUT signal (active high)
                R     -------x    State of flip-flop feeding counter #0 (active low)
    0008          W   --xxxxxx    Counter control
                  W   --x-----    NMI enable (1=enabled, 0=disabled/clear)
                  W   ---x----    CLEAR on flip-flop feeding counter #0 (active low)
                  W   ----x---    Input of flip-flop feeding counter #0
                  W   -----x--    PRESET on flip-flop feeding counter #0 (active low)
                  W   ------x-    GATE signal for counter #0 (active high)
                  W   -------x    Audio enable
    000A          W   --xxxxxx    DAC data latch (upper 6 bits)
    000B          W   xxxxxx--    DAC data latch (lower 6 bits)
    000C          W   -----xxx    CEM3394 register select
    000E          W   --xxxxxx    CEM3394 chip enable (active high)
                  W   --x-----    CEM3394 chip 0 enable
                  W   ---x----    CEM3394 chip 1 enable
                  W   ----x---    CEM3394 chip 2 enable
                  W   -----x--    CEM3394 chip 3 enable
                  W   ------x-    CEM3394 chip 4 enable
                  W   -------x    CEM3394 chip 5 enable
    ========================================================================
    Interrupts:
        INT generated by counter #2 OUT signal on 8253
        NMI generated by 6850 UART
    ========================================================================

***************************************************************************/

#include "emu.h"
#include "audio/sente6vb.h"
#include "cpu/z80/z80.h"
#include "machine/clock.h"
#include "speaker.h"


#define LOG_CEM_WRITES      0

DEFINE_DEVICE_TYPE(SENTE6VB, sente6vb_device, "sente6vb", "Bally Sente 6VB Audio Board")


/*************************************
*
*  Trivial sound device to spew
*  a MM5837 noise stream to CEM3394
*  inputs
*
*************************************/

DECLARE_DEVICE_TYPE(MM5837_NOISE_SOURCE, mm5837_noise_source)

class mm5837_noise_source : public device_t, public device_sound_interface
{
public:
	mm5837_noise_source(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
		device_t(mconfig, MM5837_NOISE_SOURCE, tag, owner, clock),
		device_sound_interface(mconfig, *this),
		m_stream(nullptr),
		m_noise_state(0x1ffff)
	{
	}

protected:
	// device-level overrides
	virtual void device_start() override
	{
		m_stream = stream_alloc(0, 1, clock());
		save_item(NAME(m_noise_state));
	}

	// sound stream update overrides
	virtual void sound_stream_update(sound_stream &stream, std::vector<read_stream_view> const &inputs, std::vector<write_stream_view> &outputs) override
	{
		for (int sampindex = 0; sampindex < outputs[0].samples(); sampindex++)
		{
			outputs[0].put(sampindex, BIT(m_noise_state, 0) ? 1.0 : 0.0);
			m_noise_state = (m_noise_state >> 1) | ((BIT(m_noise_state, 0) ^ BIT(m_noise_state, 3)) << 16);
		}
	}

private:
	sound_stream *m_stream;           // sound stream
	u32 m_noise_state;                // noise state
};

DEFINE_DEVICE_TYPE(MM5837_NOISE_SOURCE, mm5837_noise_source, "mm5837noise", "MM5837")



/*************************************
 *
 *  Sound CPU memory handlers
 *
 *************************************/

void sente6vb_device::mem_map(address_map &map)
{
	map(0x0000, 0x1fff).rom().region("audiocpu", 0);
	map(0x2000, 0x5fff).ram();
	map(0x6000, 0x6001).mirror(0x1ffe).w(m_uart, FUNC(acia6850_device::write));
	map(0xe000, 0xe001).mirror(0x1ffe).r(m_uart, FUNC(acia6850_device::read));
}


void sente6vb_device::io_map(address_map &map)
{
	map.global_mask(0xff);
	map(0x00, 0x03).rw(m_pit, FUNC(pit8253_device::read), FUNC(pit8253_device::write));
	map(0x08, 0x0f).r(FUNC(sente6vb_device::counter_state_r));
	map(0x08, 0x09).w(FUNC(sente6vb_device::counter_control_w));
	map(0x0a, 0x0b).w(FUNC(sente6vb_device::dac_data_w));
	map(0x0c, 0x0d).w(FUNC(sente6vb_device::register_addr_w));
	map(0x0e, 0x0f).w(FUNC(sente6vb_device::chip_select_w));
}


/*************************************
 *
 *  Device configuration
 *
 *************************************/

void sente6vb_device::device_add_mconfig(machine_config &config)
{
	Z80(config, m_audiocpu, 8_MHz_XTAL / 2);
	m_audiocpu->set_addrmap(AS_PROGRAM, &sente6vb_device::mem_map);
	m_audiocpu->set_addrmap(AS_IO, &sente6vb_device::io_map);

	ACIA6850(config, m_uart, 0);
	m_uart->txd_handler().set([this] (int state) { m_send_cb(state); });
	m_uart->irq_handler().set([this] (int state) { m_uint = bool(state); });

	clock_device &uartclock(CLOCK(config, "uartclock", 8_MHz_XTAL / 16)); // 500 kHz
	uartclock.signal_handler().set(FUNC(sente6vb_device::uart_clock_w));
	uartclock.signal_handler().append(m_uart, FUNC(acia6850_device::write_txc));
	uartclock.signal_handler().append(m_uart, FUNC(acia6850_device::write_rxc));

	TIMER(config, m_counter_0_timer, 0).configure_generic(FUNC(sente6vb_device::clock_counter_0_ff));

	PIT8253(config, m_pit, 0);
	m_pit->out_handler<0>().set(FUNC(sente6vb_device::counter_0_set_out));
	m_pit->out_handler<2>().set_inputline(m_audiocpu, INPUT_LINE_IRQ0);
	m_pit->set_clk<1>(8_MHz_XTAL / 4);
	m_pit->set_clk<2>(8_MHz_XTAL / 4);

	SPEAKER(config, "mono").front_center();

	mm5837_noise_source &noise(MM5837_NOISE_SOURCE(config, "noise", 100000));

	for (auto &cem_device : m_cem_device)
	{
		CEM3394(config, cem_device, 0);
		cem_device->set_vco_zero_freq(431.894);
		cem_device->set_filter_zero_freq(1300.0);
		cem_device->add_route(ALL_OUTPUTS, "mono", 0.90);
		noise.add_route(0, *cem_device, 1.0);
	}
}


/*************************************
 *
 *  ROM definition
 *
 *************************************/

ROM_START( sente6vb )
	ROM_REGION( 0x2000, "audiocpu", 0 )
	ROM_LOAD( "8002-10 9-25-84.5", 0x0000, 0x2000, CRC(4dd0a525) SHA1(f0c447adc5b67917851a9df978df851247e75c43) )
ROM_END


const tiny_rom_entry *sente6vb_device::device_rom_region() const
{
	return ROM_NAME(sente6vb);
}


/*************************************
 *
 *  Device initialization
 *
 *************************************/

sente6vb_device::sente6vb_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, SENTE6VB, tag, owner, clock),
	m_pit(*this, "pit"),
	m_counter_0_timer(*this, "8253_0_timer"),
	m_cem_device(*this, "cem%u", 1U),
	m_audiocpu(*this, "audiocpu"),
	m_uart(*this, "uart"),
	m_send_cb(*this),
	m_clock_out_cb(*this)
{
}


void sente6vb_device::device_start()
{
	m_send_cb.resolve_safe();
	m_clock_out_cb.resolve_safe();
	m_uart->write_cts(0);
	m_uart->write_dcd(0);

	save_item(NAME(m_counter_control));
	save_item(NAME(m_counter_0_ff));
	save_item(NAME(m_counter_0_out));
	save_item(NAME(m_counter_0_timer_active));

	save_item(NAME(m_dac_value));
	save_item(NAME(m_dac_register));
	save_item(NAME(m_chip_select));

	save_item(NAME(m_uint));
}


void sente6vb_device::device_reset()
{
	// reset the manual counter 0 clock
	m_counter_control = 0x00;
	m_counter_0_ff = false;
	m_counter_0_out = false;
	m_counter_0_timer_active = false;
	m_audiocpu->set_input_line(INPUT_LINE_NMI, CLEAR_LINE);

	// reset the CEM3394 I/O states
	m_dac_value = 0;
	m_dac_register = 0;
	m_chip_select = 0x3f;
}



/*************************************
 *
 *  6850 UART communications
 *
 *************************************/

WRITE_LINE_MEMBER(sente6vb_device::rec_w)
{
	m_uart->write_rxd(state);
}


WRITE_LINE_MEMBER(sente6vb_device::uart_clock_w)
{
	if (state && BIT(m_counter_control, 5))
		m_audiocpu->set_input_line(INPUT_LINE_NMI, m_uint ? ASSERT_LINE : CLEAR_LINE);

	m_clock_out_cb(!state);
}



/*************************************
 *
 *  Sound CPU counter 0 emulation
 *
 *************************************/

WRITE_LINE_MEMBER(sente6vb_device::counter_0_set_out)
{
	// OUT on counter 0 is hooked to the GATE line on counter 1 through an inverter
	m_pit->write_gate1(!state);

	// remember the out state
	m_counter_0_out = state;
}


WRITE_LINE_MEMBER(sente6vb_device::set_counter_0_ff)
{
	// the flip/flop output is inverted, so if we went high to low, that's a clock
	m_pit->write_clk0(!state);

	// remember the new state
	m_counter_0_ff = state;
}


TIMER_DEVICE_CALLBACK_MEMBER(sente6vb_device::clock_counter_0_ff)
{
	// clock the D value through the flip-flop
	set_counter_0_ff(BIT(m_counter_control, 3));
}


void sente6vb_device::update_counter_0_timer()
{
	double maxfreq = 0.0;
	int i;

	// if there's already a timer, remove it
	if (m_counter_0_timer_active)
		m_counter_0_timer->reset();
	m_counter_0_timer_active = false;

	// find the counter with the maximum frequency
	// this is used to calibrate the timers at startup
	for (i = 0; i < 6; i++)
		if (m_cem_device[i]->get_parameter(cem3394_device::FINAL_GAIN) < 10.0)
		{
			double tempfreq;

			// if the filter resonance is high, then they're calibrating the filter frequency
			if (m_cem_device[i]->get_parameter(cem3394_device::FILTER_RESONANCE) > 0.9)
				tempfreq = m_cem_device[i]->get_parameter(cem3394_device::FILTER_FREQENCY);

			// otherwise, they're calibrating the VCO frequency
			else
				tempfreq = m_cem_device[i]->get_parameter(cem3394_device::VCO_FREQUENCY);

			if (tempfreq > maxfreq) maxfreq = tempfreq;
		}

	// reprime the timer
	if (maxfreq > 0.0)
	{
		m_counter_0_timer_active = true;
		m_counter_0_timer->adjust(attotime::from_hz(maxfreq), 0, attotime::from_hz(maxfreq));
	}
}



/*************************************
 *
 *  Sound CPU counter handlers
 *
 *************************************/

uint8_t sente6vb_device::counter_state_r()
{
	// bit D0 is the inverse of the flip-flop state
	int result = !m_counter_0_ff;

	// bit D1 is the OUT value from counter 0
	if (m_counter_0_out) result |= 0x02;

	return result;
}


void sente6vb_device::counter_control_w(uint8_t data)
{
	uint8_t diff_counter_control = m_counter_control ^ data;

	// set the new global value
	m_counter_control = data;

	// bit D0 enables/disables audio
	if (BIT(diff_counter_control, 0))
	{
		for (auto & elem : m_cem_device)
			elem->set_output_gain(0, BIT(data, 0) ? 1.0 : 0);
	}

	// bit D1 is hooked to counter 0's gate
	if (BIT(diff_counter_control, 1))
	{
		// if we gate on, start a pulsing timer to clock it
		if (BIT(data, 1) && !m_counter_0_timer_active)
		{
			update_counter_0_timer();
		}

		// if we gate off, remove the timer
		else if (!BIT(data, 1) && m_counter_0_timer_active)
		{
			m_counter_0_timer->reset();
			m_counter_0_timer_active = false;
		}
	}

	// set the actual gate
	m_pit->write_gate0(BIT(data, 1));

	// bits D2 and D4 control the clear/reset flags on the flip-flop that feeds counter 0
	if (!BIT(data, 4))
		set_counter_0_ff(0);
	else if (!BIT(data, 2))
		set_counter_0_ff(1);

	// bit 5 clears the NMI interrupt
	if (BIT(diff_counter_control, 5) && !BIT(data, 5))
		m_audiocpu->set_input_line(INPUT_LINE_NMI, CLEAR_LINE);
}



/*************************************
 *
 *  CEM3394 Interfaces
 *
 *************************************/

void sente6vb_device::chip_select_w(uint8_t data)
{
	static constexpr uint8_t register_map[8] =
	{
		cem3394_device::VCO_FREQUENCY,
		cem3394_device::FINAL_GAIN,
		cem3394_device::FILTER_RESONANCE,
		cem3394_device::FILTER_FREQENCY,
		cem3394_device::MIXER_BALANCE,
		cem3394_device::MODULATION_AMOUNT,
		cem3394_device::PULSE_WIDTH,
		cem3394_device::WAVE_SELECT
	};

	double voltage = (double)m_dac_value * (8.0 / 4096.0) - 4.0;
	int diffchip = data ^ m_chip_select, i;
	int reg = register_map[m_dac_register];

	// remember the new select value
	m_chip_select = data;

	// check all six chip enables
	for (i = 0; i < 6; i++)
		if ((diffchip & (1 << i)) && (data & (1 << i)))
		{
#if LOG_CEM_WRITES
			double temp = 0;

			// remember the previous value
			temp =
#endif
				m_cem_device[i]->get_parameter(reg);

			// set the voltage
			m_cem_device[i]->set_voltage(reg, voltage);

			// only log changes
#if LOG_CEM_WRITES
			if (temp != m_cem_device[i]->get_parameter(reg))
			{
				static const char *const names[] =
				{
					"VCO_FREQUENCY",
					"FINAL_GAIN",
					"FILTER_RESONANCE",
					"FILTER_FREQENCY",
					"MIXER_BALANCE",
					"MODULATION_AMOUNT",
					"PULSE_WIDTH",
					"WAVE_SELECT"
				};
				logerror("s%04X:   CEM#%d:%s=%f\n", m_audiocpu->pcbase(), i, names[m_dac_register], voltage);
			}
#endif
		}

	// if a timer for counter 0 is running, recompute
	if (m_counter_0_timer_active)
		update_counter_0_timer();
}



void sente6vb_device::dac_data_w(offs_t offset, uint8_t data)
{
	// LSB or MSB?
	if (offset & 1)
		m_dac_value = (m_dac_value & 0xfc0) | ((data >> 2) & 0x03f);
	else
		m_dac_value = (m_dac_value & 0x03f) | ((data << 6) & 0xfc0);

	// if there are open channels, force the values in
	if ((m_chip_select & 0x3f) != 0x3f)
	{
		uint8_t temp = m_chip_select;
		chip_select_w(0x3f);
		chip_select_w(temp);
	}
}


void sente6vb_device::register_addr_w(uint8_t data)
{
	m_dac_register = data & 7;
}
