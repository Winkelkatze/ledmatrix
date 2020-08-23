/*
 * MIT License
 *
 * Copyright (c) 2020 Daniel Frejek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include <esp_heap_caps.h>
#include <esp_err.h>
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"

#include "i2s_parallel.h"

/*
 * The internal positions of the bits in the data stream buffer are always fixed.
 * Mapping is done through the GPIO matrix
 * The color values are all packed into the first byte.
 * Since these values are the only thing that changes when the images is updated, this saves some time
*/
#define BITSTREAM_COLOR_START_IO 0
#define BITSTREAM_COLOR_BYTE     0
#define BITSTREAM_COLOR_R1_POS   0
#define BITSTREAM_COLOR_G1_POS   1
#define BITSTREAM_COLOR_B1_POS   2
#define BITSTREAM_COLOR_R2_POS   3
#define BITSTREAM_COLOR_G2_POS   4
#define BITSTREAM_COLOR_B2_POS   5

#define BITSTREAM_CTRL_BYTE          1
#define BITSTREAM_CTRL_OE_BIT        0
#define BITSTREAM_CTRL_LAT_BIT       1
#define BITSTREAM_CTRL_ROW_START_BIT 2

#define BITSTREAM_CTRL_OE_IO         8  // fast
#define BITSTREAM_CTRL_LAT_IO        9
#define BITSTREAM_CTRL_ROW_START_IO  10

#define BITSTREAM_ROWS_MAX 6

#define I2S_CHN I2S_NUM_0

// The DMA length filed is 12 bit long and transfers must be word aligned
#define DMA_MAX_XFER_SIZE ((1<<12) - 4)

#define COLOR_RGB565 0
#define COLOR_GS8    1
#define COLOR_MONO   2


//#define DEBUG
//#define DEBUG_DMA
//#define DEBUG_TEST_ON_INIT

typedef struct
{
	uint8_t *stream_data;
	lldesc_t *dma_desc;
} stream_buffer_t;

struct
{
	// Buffers for the bitstreams, if not double buffered, the second is not used
	stream_buffer_t buffer[2];

	// Number of dma descriptors, equal for both buffers
	size_t dma_desc_count;

	uint16_t width;
	uint16_t height;

	// Global brightness
	// For every line, the driver output is only kept on as long as the current pixel < brightness
	uint16_t brightness;

	// Color for monochrome images
	uint8_t mono_color[3];

	// Effective number of rows, this is half of the height for displays that are split into two parts
	uint8_t rows;

	// Number of bits per color
	uint8_t color_depth;

	// index of the current backbuffer
	uint8_t backbuffer;

	// invert output signals
	bool invert;

	// swap every second column
	// Some displays are wired that way
	bool column_swap;

	// Use two buffers, this avoids tearing, but costs double the RAM
	bool double_buffer;

	// Single channel display only uses a single color channel
	// Number of rows is equal to the height for this type
	bool single_chn;

	// DMA is running / initialized
	bool initialized;
} matrix = {0};

typedef uint8_t (*get_color_func)(uint16_t x, uint16_t y, uint8_t bit, uint8_t *data);


static void start_dma()
{
	uint8_t buf = 0;
	if (matrix.double_buffer)
	{
		buf = matrix.backbuffer ^ 1;
	}
	esp_err_t err = i2s_parallel_send_dma(I2S_CHN, &matrix.buffer[buf].dma_desc[0]);
	if (err != ESP_OK)
	{
		mp_raise_OSError(err);
	}
}

static void stop_dma()
{
	// Sends a 'safe' value as last status.
	// We want to assert the OE line to blank the screen.
	// Usually the displays are safe so they don't burn out when the signal stops, but better safe than sorry.
	lldesc_t dma;
	uint8_t buffer[2];
	buffer[BITSTREAM_COLOR_BYTE] = 0;
	buffer[BITSTREAM_CTRL_BYTE] = (1 << BITSTREAM_CTRL_OE_BIT);

	if (matrix.invert)
	{
		buffer[0] = ~buffer[0];
		buffer[1] = ~buffer[1];
	}

	dma.buf = buffer;
	dma.empty = 0;
	dma.eof = 1;
	dma.length = 2;
	dma.size = 2;
	dma.offset = 0;
	dma.owner = 1;
	dma.sosf = 0;

	i2s_parallel_send_dma(I2S_CHN, &dma);

	// wait transaction finished
	while(!i2s_parallel_get_dev(I2S_CHN)->state.tx_idle);
}

static void deinit()
{
	if (matrix.initialized)
	{
		stop_dma();
	}
	if (matrix.buffer[0].stream_data) free(matrix.buffer[0].stream_data);
	if (matrix.buffer[0].dma_desc) free(matrix.buffer[0].dma_desc);
	if (matrix.buffer[1].stream_data) free(matrix.buffer[1].stream_data);
	if (matrix.buffer[1].dma_desc) free(matrix.buffer[1].dma_desc);
    memset(&matrix, 0, sizeof(matrix));
}

static void initialize_buffer(stream_buffer_t *buf)
{
	// Two bytes per pixel
	size_t subimage_stride = sizeof(uint16_t) * matrix.width * matrix.rows;
	size_t buffersize = subimage_stride * matrix.color_depth;
	size_t dma_entries_per_subimage = ((subimage_stride - 1) / DMA_MAX_XFER_SIZE) + 1;
	matrix.dma_desc_count = ((1 << matrix.color_depth) - 1) * dma_entries_per_subimage;

	buf->stream_data = heap_caps_malloc(buffersize, MALLOC_CAP_DMA);
	buf->dma_desc = heap_caps_malloc(matrix.dma_desc_count * sizeof(buf->dma_desc[0]), MALLOC_CAP_DMA);

	if (!buf->stream_data || !buf->dma_desc)
	{
		mp_raise_OSError(ESP_ERR_NO_MEM);
	}

#ifdef DEBUG
	printf("stream %u bytes @%08X\n", buffersize, (uint32_t)buf->stream_data);
	printf("dma desc %u bytes @%08X\n", matrix.dma_desc_count * sizeof(buf->dma_desc[0]), (uint32_t)buf->dma_desc);
#endif

	memset(buf->stream_data, matrix.invert ? 0xff : 0, buffersize);
	memset(buf->dma_desc, 0, matrix.dma_desc_count * sizeof(buf->dma_desc[0]));


	/*
	 * Spread the subimages evenly across the buffer to avoid flickering at lower framerates
	 * so instead of
	 * 1 2 2 3 3 3 3 4 4 4 4 4 4 4 4
	 * we want something like
	 * 4 2 4 3 4 3 1 4 2 4 3 4 3 4 4

	 * We first fill all but the last level.
	 * All remaining elements will later on be filled with the longest subimage
	 * The whole process gets a bit more complicated, since the max dma transfer size per block is limited.
	 * So to begin with, we just fill the first blocks. The sizes and other blocks are filled later.
	 */
	for (size_t i = 0; i < matrix.color_depth - 1; i++)
	{
		size_t n = 1 << i;
		for (size_t k = 0; k < n; k++)
		{
			size_t pos = (matrix.dma_desc_count * k) / n + (matrix.dma_desc_count / n / 2);

			// in case we need more than one dma desc per image, we fill only the first element
			pos /= dma_entries_per_subimage;
			pos *= dma_entries_per_subimage;

#ifdef DEBUG_DMA
			printf("level=%u n=%u k=%u pos=%u\n", i, n, k, pos);
#endif

			// Find next free entry, wrap around if required
			while (buf->dma_desc[pos].buf)
			{
				pos += dma_entries_per_subimage;
				if (pos >= matrix.dma_desc_count)
				{
					pos = 0;
				}
			}

			buf->dma_desc[pos].buf = buf->stream_data + subimage_stride * i;
#ifdef DEBUG_DMA
			printf("  -> %u=%08X\n", pos, (uint32_t)buf->dma_desc[pos].buf);
#endif
		}
	}

	// Fill remaining elements and create links + fill common data
	for (size_t i = 0; i < matrix.dma_desc_count; i++)
	{
		if (!buf->dma_desc[i].buf)
		{
			buf->dma_desc[i].buf = buf->stream_data + subimage_stride * (matrix.color_depth - 1);
		}

		size_t remaining = subimage_stride;
		volatile uint8_t *ptr = buf->dma_desc[i].buf;
		--i;
		while (remaining)
		{
			++i;

			size_t block = remaining;
			if (block > DMA_MAX_XFER_SIZE)
			{
				block = DMA_MAX_XFER_SIZE;
			}

#ifdef DEBUG_DMA
			printf("dma %u=%08X length=%u\n", i, (uint32_t)ptr, block);
#endif
			buf->dma_desc[i].buf = ptr;
			buf->dma_desc[i].length = block;
			buf->dma_desc[i].size = block;
			buf->dma_desc[i].owner = 1;
			ptr += block;
			remaining -= block;
		}
	}

	for (size_t i = 0; i < matrix.dma_desc_count - 1; i++)
	{
		// link to next
		buf->dma_desc[i].qe.stqe_next = &buf->dma_desc[i + 1];
	}

	//close the loop
	buf->dma_desc[matrix.dma_desc_count - 1].qe.stqe_next = &buf->dma_desc[0];
}


/*
 * Creates the control sequence for selecting the display lines and the latching.
 * This also handles the global brightness setting
 */
void create_control_pattern(stream_buffer_t *buf)
{
	size_t row_stride = sizeof(uint16_t) * matrix.width;
	size_t subimage_stride = row_stride * matrix.rows;
	for (uint8_t lvl = 0; lvl < matrix.color_depth; lvl++)
	{
		uint8_t *si = buf->stream_data + subimage_stride * lvl;
		for (uint8_t row = 0; row < matrix.rows; row++)
		{
			uint8_t *r = si + row_stride * row;

			// The row lines control the currently shown row.
			// this is always the last row as the current row is being filled with new data
			uint8_t display_row = row - 1; // the possible underflow IS expected and desired!

			for (uint16_t pixel = 0; pixel < matrix.width; pixel++)
			{
				uint8_t *px = r + sizeof(uint16_t) * pixel;

				uint8_t ctrl = display_row << BITSTREAM_CTRL_ROW_START_BIT;

				if (pixel < 2 || pixel > matrix.brightness)
				{
					// Disable the led drivers while switching rows. We also use this to control
					// the global brightness by blanking the screen after transmitting n pixels.
					// NOTE: The OE line is active low, BLANK would be a more suiting name...
					ctrl |= 1 << BITSTREAM_CTRL_OE_BIT;
				}

				if (pixel == matrix.width - 2)
				{
					// Latch when transmitting the last pixel
					// NOTE: This is somewhat problematic, since we are latching while the clock is
					//       still running and we are still loading fresh data into the shift registers.
					//       Asserting the latch with the second last pixel (and thus having the falling edge on the last) seems to work reliable though.
					ctrl |= 1 << BITSTREAM_CTRL_LAT_BIT;
				}

				if (matrix.invert)    ctrl = ~ctrl;
				px[BITSTREAM_CTRL_BYTE] = ctrl;
			}
		}
	}
}

#ifdef DEBUG_TEST_ON_INIT
static uint8_t get_color_bits_test(uint16_t x, uint16_t y, uint8_t bit, uint8_t *data)
{
	(void)data;
	(void)bit;
	if ((x + y) % 4 == 3) return 0;
	return 9 << ((x + y) % 4);
}
#endif

static uint8_t get_rgb565_bits(uint16_t color, uint8_t bit)
{
	// expand to 3x8 bits
	uint8_t r = (color >> 8) & 0xf8;
	uint8_t g = (color >> 3) & 0xfc;
	uint8_t b = (color << 3);

	return (
		((r >> (7 - bit)) & 1) |
		(((g >> (7 - bit)) & 1) << 1) |
		(((b >> (7 - bit)) & 1) << 2)
		);
}

static uint8_t get_mono_color_bits(uint8_t bit)
{
	return (
		(( matrix.mono_color[0] >> (7 - bit)) & 1) |
		(((matrix.mono_color[1] >> (7 - bit)) & 1) << 1) |
		(((matrix.mono_color[2] >> (7 - bit)) & 1) << 2)
		);
}

static uint8_t get_color_bits_mono_hlsb(uint16_t x, uint16_t y, uint8_t bit, uint8_t *data)
{
	if (data[(x >> 3) + ((y * matrix.width) >> 3)] & (0x80 >> (1 - (x & 7))))
	{
		return get_mono_color_bits(bit);
	}
	return 0;
}

static uint8_t get_color_bits_rgb(uint16_t x, uint16_t y, uint8_t bit, uint8_t *data)
{
	uint16_t val = ((uint16_t *)data)[y * matrix.width + x];
	return get_rgb565_bits(val, bit);
}

static uint8_t get_color_bits_gs8(uint16_t x, uint16_t y, uint8_t bit, uint8_t *data)
{
	uint16_t v = data[(y * matrix.width + x)];

	uint8_t r = (uint8_t)((v * matrix.mono_color[0]) / 255);
	uint8_t g = (uint8_t)((v * matrix.mono_color[1]) / 255);
	uint8_t b = (uint8_t)((v * matrix.mono_color[2]) / 255);

	return (
		((r >> (7 - bit)) & 1) |
		(((g >> (7 - bit)) & 1) << 1) |
		(((b >> (7 - bit)) & 1) << 2)
		);
}

static void update_framebuffer(stream_buffer_t *buf, uint8_t *data, get_color_func get_color_bits)
{
	size_t row_stride = sizeof(uint16_t) * matrix.width;
	size_t subimage_stride = row_stride * matrix.rows;
	for (uint8_t lvl = 0; lvl < matrix.color_depth; lvl++)
	{
		uint8_t *si = buf->stream_data + subimage_stride * lvl;
		for (uint8_t row = 0; row < matrix.rows; row++)
		{
			uint8_t *r = si + row_stride * row;
			for (uint16_t pixel = 0; pixel < matrix.width; pixel++)
			{
				uint8_t *px = r + sizeof(uint16_t) * pixel;

				uint16_t source_px = pixel;
				if (matrix.column_swap) source_px ^= 0x01;
				uint8_t bit = matrix.color_depth - lvl - 1;
				uint8_t c = get_color_bits(source_px, row, bit, data);
				if (!matrix.single_chn)
				{
					c |= get_color_bits(source_px, row + matrix.rows, bit, data) << 3;
				}
				if (matrix.invert) c = ~c;
				px[BITSTREAM_COLOR_BYTE] = c;
			}
		}
	}
}


/*
 * Initialize the led matrix driver
 * Parameters are:
 * io_colors
 *    GPIO lines for the color inputs of the matrix
 *    The order is R1 G1 B1 [R2 G2 B2]
 *    The *2 values are only required in the double channel (default) mode.
 * io_rows
 *    GPIO lines for the row inputs of the matrix
 *    The order is from LSB to MSB. On most displays these are named A B C D ...
 *    The display height is implicitly defined by the number of rows
 * io_oe
 *    GPIO line for the BLANK / OE (Output enable) input.
 * io_lat
 *    GPIO line for the LAT (latch) input
 * io_clk
 *    GPIO line for the CLK (clock) input
 * width
 *    Width of the display.
 *    If multiple segments are chained, the width is just extended as if it would be one longer display.
 * color_depth, default=4
 *    Number of bits per color channel.
 *    A higher color depth requires a higher clock to be flicker-free.
 * clock_speed_khz, default=2500
 *    Clock speed of the output. Must be between 313 and 40000.
 * invert, default=False
 *     Invert the output signal for use with inverting level shifters.
 * double_buffer, default=False
 *     Use double buffering for tearing free updates.
 *     This doubles the memory requirement.
 * column_swap, default=True
 *     Swap the output for every second column, since on many displays these are swapped internally.
 * single_channel, default=False
 *     Single channel display with only three color lines.
 *     Most displays are split vertically into an upper and lower half with two separate sets of color lines.
 * brightness, default=width-2
 *     Global brightness control.
 *     Must be between 0 (off) and width - 2 (max)
 */
STATIC mp_obj_t ledmatrix_init(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	deinit();

	// The height is implicitly defined by the number of rows
	static const mp_arg_t allowed_args[] = {
	/*  0 */ { MP_QSTR_io_colors,       MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none}},
	/*  1 */ { MP_QSTR_io_rows,         MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none}},
	/*  2 */ { MP_QSTR_io_oe,           MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
	/*  3 */ { MP_QSTR_io_lat,          MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
	/*  4 */ { MP_QSTR_io_clk,          MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
	/*  5 */ { MP_QSTR_width,           MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
	/*  6 */ { MP_QSTR_color_depth,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 4}},
	/*  7 */ { MP_QSTR_clock_speed_khz, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2500}},
	/*  8 */ { MP_QSTR_invert,          MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
	/*  9 */ { MP_QSTR_double_buffer,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
	/* 10 */ { MP_QSTR_column_swap,     MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
	/* 11 */ { MP_QSTR_single_channel,  MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
	/* 12 */ { MP_QSTR_brightness,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1}},
	};

	mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

	matrix.width = args[5].u_int;
	matrix.color_depth = args[6].u_int;
	matrix.invert = args[8].u_bool;
	matrix.double_buffer = args[9].u_bool;
	matrix.column_swap = args[10].u_bool;
	matrix.single_chn = args[11].u_bool;

	if (matrix.width & 1)
	{
		// I don't want to deal with padding for the DMA transfers...
		mp_raise_ValueError(MP_ERROR_TEXT("width must be an even number"));
	}

	if (args[12].u_int > 0)
	{
		matrix.brightness = args[12].u_int;
		if (matrix.brightness >= matrix.width - 1)
		{
			mp_raise_ValueError(MP_ERROR_TEXT("Brightness must be between 0 and width - 2"));
		}
		matrix.brightness++;
	}
	else
	{
		matrix.brightness = matrix.width - 1;
	}

	if (matrix.color_depth == 0)
	{
		mp_raise_ValueError(MP_ERROR_TEXT("invalid value for color depth"));
	}

	i2s_parallel_config_t cfg;
	cfg.sample_width = I2S_PARALLEL_WIDTH_16;

	for (size_t i = 0; i < MP_ARRAY_SIZE(cfg.gpios_bus); i++)
	{
		cfg.gpios_bus[i] = -1;
	}

	cfg.gpios_bus[BITSTREAM_CTRL_OE_IO] = args[2].u_int;
	cfg.gpios_bus[BITSTREAM_CTRL_LAT_IO] = args[3].u_int;
	cfg.gpio_clk = args[4].u_int;
	cfg.sample_rate = args[7].u_int * 1000; // parameter is in khz


	size_t iteridx = 0;
	mp_obj_iter_buf_t iter_buf;
	mp_obj_t item, iterable = mp_getiter(args[0].u_obj, &iter_buf);
	while ((item = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
		if (iteridx >= 6)
		{
			break;
		}

		if (!mp_obj_is_small_int(item))
		{
			mp_raise_TypeError(MP_ERROR_TEXT("values of io_colors must be ints"));
		}

		cfg.gpios_bus[BITSTREAM_COLOR_START_IO + iteridx] = MP_OBJ_SMALL_INT_VALUE(item);
		iteridx++;
	}
	if ((matrix.single_chn && iteridx != 3) || (!matrix.single_chn && iteridx != 6))
	{
		mp_raise_ValueError(MP_ERROR_TEXT("Unexpected number of color io lines"));
	}

	iteridx = 0;

	iterable = mp_getiter(args[1].u_obj, &iter_buf);
	while ((item = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
		if (iteridx >= BITSTREAM_ROWS_MAX)
		{
			mp_raise_ValueError(MP_ERROR_TEXT("Too many values given for io_rows"));
		}

		if (!mp_obj_is_small_int(item))
		{
			mp_raise_TypeError(MP_ERROR_TEXT("values of io_rows must be ints"));
		}

		cfg.gpios_bus[BITSTREAM_CTRL_ROW_START_IO + iteridx] = MP_OBJ_SMALL_INT_VALUE(item);
		iteridx++;
	}

	// number of 'rows' in the display
	// For most displays, the height of the display is twice the number of 'rows' since the display is split into two halves
	// There are some small / old versions, that only have a single channel
	matrix.rows = 1U << iteridx;

	if (matrix.single_chn)
	{
		matrix.height = matrix.rows;
	}
	else
	{
		matrix.height = matrix.rows << 1;
	}

	matrix.backbuffer = 0;
	matrix.mono_color[0] = 0xff;
	matrix.mono_color[1] = 0xff;
	matrix.mono_color[2] = 0xff;

#ifdef DEBUG
	printf("I2S config: io_clk=%i, rate=%i gpio:\n", cfg.gpio_clk, cfg.sample_rate);
	for(size_t i = 0; i < 16; i++)
	{
		printf("%i ", cfg.gpios_bus[i]);
	}
	printf("\nSize %ix%i : %i\n", matrix.width, matrix.height, matrix.color_depth);
	printf("brightness %i\n", matrix.brightness);
	printf("swap %i\n", matrix.column_swap);
	printf("invert %i\n", matrix.invert);
	printf("doublebuffer %i\n", matrix.double_buffer);
#endif

	initialize_buffer(&matrix.buffer[0]);
	create_control_pattern(&matrix.buffer[0]);
	if (matrix.double_buffer)
	{
		initialize_buffer(&matrix.buffer[1]);
		create_control_pattern(&matrix.buffer[1]);
		matrix.backbuffer = 1;
	}

#ifdef DEBUG_TEST_ON_INIT
	update_framebuffer(&matrix.buffer[0], NULL, &get_color_bits_test);
#endif

	esp_err_t err = i2s_parallel_driver_install(I2S_CHN, &cfg, matrix.invert, NULL, NULL);
	if (err != ESP_OK)
	{
		mp_raise_OSError(err);
	}

	matrix.initialized = true;

	start_dma();

	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(ledmatrix_init_obj, 0, ledmatrix_init);


/*
 * Set the global brightness
 * Value must be between 0 (off) and width - 2 (max)
 */
STATIC mp_obj_t ledmatrix_set_brightness(mp_obj_t b)
{
	if (!matrix.initialized)
		mp_raise_ValueError(MP_ERROR_TEXT("ledmatrix not initialized"));
	if (!mp_obj_is_small_int(b))
		mp_raise_TypeError(MP_ERROR_TEXT("expected small int"));

	int newb = MP_OBJ_SMALL_INT_VALUE(b);

	if (newb < 0 || newb >= matrix.width - 1)
		mp_raise_ValueError(MP_ERROR_TEXT("Brightness must be between 0 and width - 2"));

	matrix.brightness = newb + 1;

	// This somewhat bypasses the double buffer feature,
	// but the global brightness control is not really intended to be used frequent
	// Also, it won't induce any tearing etc...
	create_control_pattern(&matrix.buffer[0]);
	if (matrix.double_buffer)
	{
		create_control_pattern(&matrix.buffer[1]);
	}
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(ledmatrix_set_brightness_obj, ledmatrix_set_brightness);

/*
 * Update the internal framebuffer from the specified data
 * Parameters are
 * fb
 *     Framebuffer
 *     The format must be one of the following:
 *         RGB565
 *         GS8
 *         MONO_HLSB
 * mode, default=RGB565
 *    Format of the framebuffer, values are the COLOR_* constants
 *    Must be matching the format of the specified framebuffer
 * mono_color, optional
 *     Color to use for monochrome images.
 */
STATIC mp_obj_t ledmatrix_show(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
	if (!matrix.initialized)
		mp_raise_ValueError(MP_ERROR_TEXT("ledmatrix not initialized"));

	static const mp_arg_t allowed_args[] = {
		{ MP_QSTR_fb, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_int = 0} },
		{ MP_QSTR_mono_color, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
		{ MP_QSTR_mode, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COLOR_RGB565} },
	};

	mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

	mp_buffer_info_t src;
	mp_get_buffer_raise(args[0].u_obj, &src, MP_BUFFER_READ);

	int color = args[1].u_int;
	if (color >= 0)
	{
		matrix.mono_color[0] = (color >> 16) & 0xff;
		matrix.mono_color[1] = (color >> 8) & 0xff;
		matrix.mono_color[2] = color & 0xff;
	}

	stream_buffer_t *strm = &matrix.buffer[matrix.backbuffer];

	switch (args[2].u_int)
	{
		case COLOR_RGB565:
			if (src.len != matrix.width * matrix.height * 2)
			{
				mp_raise_ValueError(MP_ERROR_TEXT("Unexpected buffer size"));
			}
			update_framebuffer(strm, (uint8_t*)src.buf, &get_color_bits_rgb);
			break;
		case COLOR_GS8:
			if (src.len != matrix.width * matrix.height)
			{
				mp_raise_ValueError(MP_ERROR_TEXT("Unexpected buffer size"));
			}
			update_framebuffer(strm, (uint8_t*)src.buf, &get_color_bits_gs8);
			break;
		case COLOR_MONO:
			if (src.len != (((matrix.width - 1) / 8) + 1) * matrix.height)
			{
				mp_raise_ValueError(MP_ERROR_TEXT("Unexpected buffer size"));
			}
			update_framebuffer(strm, (uint8_t*)src.buf, &get_color_bits_mono_hlsb);
			break;
	}

	if (matrix.double_buffer)
	{
		// Close loop for new frontbuffer and redirect running DMA transaction
		matrix.buffer[0].dma_desc[matrix.dma_desc_count - 1].qe.stqe_next = &matrix.buffer[matrix.backbuffer].dma_desc[0];
		matrix.buffer[1].dma_desc[matrix.dma_desc_count - 1].qe.stqe_next = &matrix.buffer[matrix.backbuffer].dma_desc[0];
		matrix.backbuffer ^= 1;
	}

	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(ledmatrix_show_obj, 1, ledmatrix_show);

/*
 * Blank the screen and stop the data output to the display.
 * Buffers are kept and can be changed while the display is off.
 */
STATIC mp_obj_t ledmatrix_stop()
{
	if (!matrix.initialized)
		mp_raise_ValueError(MP_ERROR_TEXT("ledmatrix not initialized"));
	stop_dma();
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ledmatrix_stop_obj, ledmatrix_stop);

/*
 * Resume outputting data to the display.
 */
STATIC mp_obj_t ledmatrix_resume()
{
	if (!matrix.initialized)
		mp_raise_ValueError(MP_ERROR_TEXT("ledmatrix not initialized"));
	start_dma();
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ledmatrix_resume_obj, ledmatrix_resume);

/*
 * Turn off the screen and deinitialize the display driver. All buffers are freed.
 */
STATIC mp_obj_t ledmatrix_deinitialize()
{
	deinit();
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ledmatrix_deinitialize_obj, ledmatrix_deinitialize);

STATIC const mp_rom_map_elem_t ledmatrix_module_globals_table[] = {
		{ MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_ledmatrix) },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_init), (mp_obj_t)&ledmatrix_init_obj },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_set_brightness), (mp_obj_t)&ledmatrix_set_brightness_obj },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_show), (mp_obj_t)&ledmatrix_show_obj },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_stop), (mp_obj_t)&ledmatrix_stop_obj },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_resume), (mp_obj_t)&ledmatrix_resume_obj },
		{ MP_OBJ_NEW_QSTR(MP_QSTR_deinitialize), (mp_obj_t)&ledmatrix_deinitialize_obj },
		{ MP_ROM_QSTR(MP_QSTR_FB_RGB565), MP_ROM_INT(COLOR_RGB565) },
		{ MP_ROM_QSTR(MP_QSTR_FB_GS8), MP_ROM_INT(COLOR_GS8) },
		{ MP_ROM_QSTR(MP_QSTR_FB_MONO), MP_ROM_INT(COLOR_MONO) },
};

STATIC MP_DEFINE_CONST_DICT(ledmatrix_module_globals, ledmatrix_module_globals_table);

const mp_obj_module_t ledmatrix_user_cmodule = {
		.base = { &mp_type_module },
		.globals = (mp_obj_dict_t*)&ledmatrix_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ledmatrix, ledmatrix_user_cmodule, MODULE_LEDMATRIX_ENABLED);
