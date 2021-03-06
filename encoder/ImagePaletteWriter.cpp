/*
	Copyright (c) 2013 Game Closure.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of GCIF nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#include "ImagePaletteWriter.hpp"
#include "../decoder/EndianNeutral.hpp"
#include "Log.hpp"
#include "../decoder/Filters.hpp"
#include "EntropyEstimator.hpp"
#include "EntropyEncoder.hpp"
#include "PaletteOptimizer.hpp"
using namespace cat;
using namespace std;


#ifdef CAT_DESYNCH_CHECKS
#define DESYNC_TABLE() writer.writeWord(1234567);
#define DESYNC(x, y) writer.writeBits(x ^ 12345, 16); writer.writeBits(y ^ 54321, 16);
#else
#define DESYNC_TABLE()
#define DESYNC(x, y)
#endif


//// ImagePaletteWriter

bool ImagePaletteWriter::generatePalette() {
	u32 hist[PALETTE_MAX] = {0};

	const u32 *color = reinterpret_cast<const u32 *>( _rgba );
	int palette_size = 0;

	for (int y = 0; y < _ysize; ++y) {
		for (int x = 0, xend = _xsize; x < xend; ++x) {
			u32 c = *color++;

			if (IsMasked(x, y)) {
				continue;
			}

			// Determine palette index
			int index;
			if (_map.find(c) == _map.end()) {
				// If ran out of palette slots,
				if (palette_size >= PALETTE_MAX) {
					return false;
				}

				index = palette_size;
				_map[c] = index;
				_palette.push_back(c);

				++palette_size;
			} else {
				index = _map[c];
			}

			// Record how often each palette index is used
			hist[index]++;
		}
	}

	// If palette size is degenerate,
	if (palette_size <= 0) {
		CAT_DEBUG_EXCEPTION();
		return false;
	}

	// Store the palette size
	_palette_size = palette_size;

	// Record the most common color
	int best_index = 0;
	u32 best_count = 0;
	for (int index = 0; index < palette_size; ++index) {
		u32 count = hist[index];

		if (best_count < count) {
			best_count = count;
			best_index = index;
		}
	}
	_most_common = (u8)best_index;

	return true;
}

void ImagePaletteWriter::generateImage() {
	// Choose the most common color as the mask color, if it is not found
	u8 masked_palette = _most_common;
	if (_mask->enabled()) {
		u32 maskColor = _mask->getColor();

		if (_map.find(maskColor) != _map.end()) {
			masked_palette = _map[maskColor];
		}
	}
	_masked_palette = masked_palette;

	// Generate the palettized image
	const int image_size = _xsize * _ysize;
	_image.resize(image_size);

	const u32 *color = reinterpret_cast<const u32 *>( _rgba );
	u8 *image = _image.get();

	for (int y = 0; y < _ysize; ++y) {
		for (int x = 0, xend = _xsize; x < xend; ++x, ++image, ++color) {
			image[0] = _mask->masked(x, y) ? masked_palette : _map[color[0]];
		}
	}
}

void ImagePaletteWriter::optimizeImage() {
	CAT_INANE("Palette") << "Optimizing palette with " << _palette_size << " entries...";

	_optimizer.process(_image.get(), _xsize, _ysize, _palette_size,
		PaletteOptimizer::MaskDelegate::FromMember<ImagePaletteWriter, &ImagePaletteWriter::IsMasked>(this));

#ifdef CAT_DEBUG
	const u8 *p = _image.get();
	const u8 *b = _optimizer.getOptimizedImage();
	for (int y = 0; y < _ysize; ++y) {
		for (int x = 0; x < _xsize; ++x, ++p, ++b) {
			u8 index = *p;
			u8 mutated = _optimizer.forward(index);

			CAT_DEBUG_ENFORCE(*b == mutated);
		}
	}
#endif

	// Replace palette image
	const u8 *src = _optimizer.getOptimizedImage();
	memcpy(_image.get(), src, _xsize * _ysize);

	// Fix color palette array
	u32 better_palette[PALETTE_MAX];

	for (int ii = 0; ii < _palette_size; ++ii) {
		better_palette[_optimizer.forward(ii)] = _palette[ii];
	}
	memcpy(&_palette[0], better_palette, _palette_size * sizeof(_palette[0]));

	_masked_palette = _optimizer.forward(_masked_palette);

	// NOTE: We leave _map dirty since it is not used again
}

void ImagePaletteWriter::generateMonoWriter() {
	CAT_INANE("Palette") << "Compressing index matrix...";

	MonoWriter::Parameters params;

	params.knobs = _knobs;
	params.data = _image.get();
	params.num_syms = _palette_size;
	params.xsize = _xsize;
	params.ysize = _ysize;
	params.max_filters = 32;
	params.min_bits = 2;
	params.max_bits = 5;
	params.sympal_thresh = _knobs->pal_sympalThresh;
	params.filter_cover_thresh = _knobs->pal_filterCoverThresh;
	params.filter_inc_thresh = _knobs->pal_filterIncThresh;
	params.mask.SetMember<ImagePaletteWriter, &ImagePaletteWriter::IsMasked>(this);
	params.AWARDS[0] = _knobs->pal_awards[0];
	params.AWARDS[1] = _knobs->pal_awards[1];
	params.AWARDS[2] = _knobs->pal_awards[2];
	params.AWARDS[3] = _knobs->pal_awards[3];
	params.award_count = 4;
	params.write_order = 0;
	params.lz_enable = _knobs->pal_enableLZ;

	_mono_writer.init(params);
}

int ImagePaletteWriter::init(const u8 *rgba, int xsize, int ysize, const GCIFKnobs *knobs, ImageMaskWriter &mask) {
	_knobs = knobs;
	_rgba = rgba;
	_xsize = xsize;
	_ysize = ysize;
	_mask = &mask;

	// Off by default
	_palette_size = 0;

	// If palette was generated,
	if (generatePalette()) {
		// Generate palette raster
		generateImage();

		// Optimize the palette selections to improve compression
		optimizeImage();

		// Generate mono writer
		generateMonoWriter();
	}

	return GCIF_WE_OK;
}

bool ImagePaletteWriter::IsMasked(u16 x, u16 y) {
	return _mask->masked(x, y);
}

void ImagePaletteWriter::write(ImageWriter &writer) {
	if (enabled()) {
		writer.writeBit(1);
		writeTable(writer);
		writePixels(writer);
	} else {
		writer.writeBit(0);
	}
}

void ImagePaletteWriter::writeTable(ImageWriter &writer) {
	int pal_bits = 0, pal_table_bits = 1, mono_bits = 0;

	CAT_DEBUG_ENFORCE(PALETTE_MAX <= 256);

	writer.writeBits(_palette_size - 1, 8);
	pal_bits += 8;

	// Write palette index for mask
	writer.writeBits(_masked_palette, 8);
	pal_bits += 8;

	// If palette is small,
	if (_palette_size < _knobs->pal_huffThresh) {
		writer.writeBit(0);
		++pal_bits;

		for (int ii = 0; ii < _palette_size; ++ii) {
			u32 color = getLE(_palette[ii]);

			writer.writeWord(color);
			pal_bits += 32;
		}
	} else {
		writer.writeBit(1);
		++pal_bits;

		// Find best color filter
		int bestCF = 0;
		u32 bestScore = 0x7fffffff;

		EntropyEstimator ee;
		ee.init();

		SmartArray<u8> edata;
		edata.resize(_palette_size * 4);

		for (int cf = 0; cf < CF_COUNT; ++cf) {
			RGB2YUVFilterFunction filter = RGB2YUV_FILTERS[cf];

			u8 *write = edata.get();
			for (int ii = 0; ii < _palette_size; ++ii) {
				u32 color = getLE(_palette[ii]);

				u8 rgb[3] = {
					(u8)color,
					(u8)(color >> 8),
					(u8)(color >> 16)
				};

				filter(rgb, write);
				write[3] = 255 - (u8)(color >> 24);
				write += 4;
			}

			u32 entropy = ee.entropy(edata.get(), edata.size());
			if (bestScore > entropy) {
				bestScore = entropy;
				bestCF = cf;
			}
		}

		CAT_DEBUG_ENFORCE(CF_COUNT == 17);
		pal_bits += writer.write17(bestCF);

		RGB2YUVFilterFunction bestFilter = RGB2YUV_FILTERS[bestCF];

		EntropyEncoder encoder;
		encoder.init(PALETTE_MAX, ENCODER_ZRLE_SYMS);

		// Train
		for (int ii = 0; ii < _palette_size; ++ii) {
			u32 color = getLE(_palette[ii]);

			u8 rgb[3] = {
				(u8)color,
				(u8)(color >> 8),
				(u8)(color >> 16)
			};

			u8 yuva[4];
			bestFilter(rgb, yuva);
			yuva[3] = 255 - (u8)(color >> 24);

			encoder.add(yuva[0]);
			encoder.add(yuva[1]);
			encoder.add(yuva[2]);
			encoder.add(yuva[3]);
		}

		encoder.finalize();

		pal_table_bits += encoder.writeTables(writer);

		// Fire
		for (int ii = 0; ii < _palette_size; ++ii) {
			u32 color = getLE(_palette[ii]);

			u8 rgb[3] = {
				(u8)color,
				(u8)(color >> 8),
				(u8)(color >> 16)
			};

			u8 yuva[4];
			bestFilter(rgb, yuva);
			yuva[3] = 255 - (u8)(color >> 24);

			pal_bits += encoder.write(yuva[0], writer);
			pal_bits += encoder.write(yuva[1], writer);
			pal_bits += encoder.write(yuva[2], writer);
			pal_bits += encoder.write(yuva[3], writer);
		}
	}

	DESYNC_TABLE();

	// Write monochrome tables
	mono_bits += _mono_writer.writeTables(writer);

	DESYNC_TABLE();

#ifdef CAT_COLLECT_STATS
	Stats.palette_size = _palette_size;
	Stats.pal_overhead_bits = pal_bits;
	Stats.pal_table_bits = pal_table_bits;
	Stats.mono_overhead_bits = mono_bits;
#endif
}

void ImagePaletteWriter::writePixels(ImageWriter &writer) {
	int bits = 0, pixels = 0;

	for (int y = 0; y < _ysize; ++y) {
		bits += _mono_writer.writeRowHeader(y, writer);

		for (int x = 0; x < _xsize; ++x) {
			DESYNC(x, y);

			if (IsMasked(x, y)) {
				_mono_writer.zero(x);
			} else {
				bits += _mono_writer.write(x, y, writer);
				++pixels;
			}
		}
	}

#ifdef CAT_COLLECT_STATS
	Stats.mono_bits = bits;
	Stats.total_bits = Stats.pal_overhead_bits + Stats.pal_table_bits + Stats.mono_overhead_bits + bits;
	Stats.pixel_count = pixels;
	Stats.file_bits = Stats.total_bits + _mask->Stats.compressedDataBits;
	Stats.original_bits = Stats.pixel_count * 32;
	Stats.pixel_compression_ratio = Stats.original_bits / (float)Stats.total_bits;
	Stats.file_compression_ratio = _xsize * _ysize * 32 / (float)Stats.file_bits;
#endif
}


#ifdef CAT_COLLECT_STATS

bool ImagePaletteWriter::dumpStats() {
	if (!enabled()) {
		CAT_INANE("stats") << "(Palette) Disabled.";
	} else {
		_mono_writer.dumpStats();

		CAT_INANE("stats") << "(Palette compress)      Palette Size : " << Stats.palette_size << " colors";
		CAT_INANE("stats") << "(Palette compress)     Palette Table : " << Stats.pal_table_bits / 8 << " bytes (" << Stats.pal_table_bits * 100.f / Stats.total_bits << "% total)";
		CAT_INANE("stats") << "(Palette compress)  Palette Overhead : " << Stats.pal_overhead_bits / 8 << " bytes (" << Stats.pal_overhead_bits * 100.f / Stats.total_bits << "% total)";
		CAT_INANE("stats") << "(Palette compress)  Monochrome Table : " << Stats.mono_overhead_bits / 8 << " bytes (" << Stats.mono_overhead_bits * 100.f / Stats.total_bits << "% total)";
		CAT_INANE("stats") << "(Palette compress)            Pixels : " << Stats.mono_bits / 8 << " bytes (" << Stats.mono_bits * 100.f / Stats.total_bits << "% total)";
		CAT_INANE("stats") << "(Palette compress) Compression Ratio : " << Stats.pixel_compression_ratio << ":1 RGBA pixels";
		CAT_INANE("stats") << "(Palette compress)  Total Pixel Data : " << Stats.total_bits/8 << " bytes (" << Stats.total_bits * 100.f / Stats.file_bits << " % of file)";
		CAT_INANE("stats") << "(Palette compress)        File Ratio : " << Stats.file_compression_ratio << ":1";
	}

	return true;
}

#endif

