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

#include "ImageRGBAWriter.hpp"
#include "../decoder/BitMath.hpp"
#include "../decoder/Filters.hpp"
#include "EntropyEstimator.hpp"
#include "Log.hpp"
#include "ImageLZWriter.hpp"
using namespace cat;

#include <vector>
using namespace std;

#include "../decoder/lz4.h"
#include "lz4hc.h"
#include "Log.hpp"
#include "HuffmanEncoder.hpp"
#include "lodepng.h"

#ifdef CAT_DESYNCH_CHECKS
#define DESYNC_TABLE() writer.writeBits(1234567);
#define DESYNC(x, y) writer.writeBits(x ^ 12345, 16); writer.writeBits(y ^ 54321, 16);
#define DESYNC_FILTER(x, y) writer.writeBits(x ^ 31337, 16); writer.writeBits(y ^ 31415, 16);
#else
#define DESYNC_TABLE()
#define DESYNC(x, y)
#define DESYNC_FILTER(x, y)
#endif


//// ImageRGBAWriter

void ImageRGBAWriter::maskTiles() {
	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _size_x, size_y = _size_y;
	u8 *sf = _sf_tiles.get();
	u8 *cf = _cf_tiles.get();

	// For each tile,
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		for (u16 x = 0; x < size_x; x += tile_size_x) {

			// For each element in the tile,
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If it is not masked,
					if (!IsMasked(px, py)) {
						// We need to do this tile
						*cf++ = TODO_TILE;
						*sf++ = TODO_TILE;
						goto next_tile;
					}
				}
				++py;
			}

			// Tile is masked out entirely
			*cf++ = MASK_TILE;
			*sf++ = MASK_TILE;
next_tile:;
		}
	}
}

void ImageRGBAWriter::designFilters() {
	FilterScorer scores, awards;
	scores.init(SF_COUNT);
	awards.init(SF_COUNT);
	awards.reset();
	u8 FPT[3];

	CAT_INANE("RGBA") << "Designing spatial filters...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _size_x, size_y = _size_y;

	u8 *sf = _sf_tiles.get();
	u8 *cf = _cf_tiles.get();
	const u8 *topleft = _rgba;
	for (int y = 0; y < _size_y; y += _tile_size_y) {
		for (int x = 0; x < _size_x; x += _tile_size_x, ++sf, ++cf, topleft += _tile_size_x * 4) {
			if (*sf == MASK_TILE) {
				continue;
			}

			scores.reset();

			// For each element in the tile,
			const u8 *row = topleft;
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				const u8 *data = row;
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If element is not masked,
					if (!IsMasked(px, py)) {
						const u8 r = data[0];
						const u8 g = data[1];
						const u8 b = data[2];

						for (int f = 0; f < SF_COUNT; ++f) {
							const u8 *pred = RGBA_FILTERS[f].safe(data, FPT, x, y, size_x);

							u8 rr = r - pred[0];
							u8 rg = g - pred[1];
							u8 rb = b - pred[2];

							int score = RGBAChaos::ResidualScore(rr);
							score += RGBAChaos::ResidualScore(rg);
							score += RGBAChaos::ResidualScore(rb);

							scores.add(f, score);
						}
					}
					data += 4;
				}
				++py;
				row += size_x * 4;
			}

			FilterScorer::Score *top = scores.getTop(4, true);
			awards.add(top[0].index, 5);
			awards.add(top[1].index, 3);
			awards.add(top[2].index, 1);
			awards.add(top[3].index, 1);
		}
	}

	// Copy fixed functions
	for (int jj = 0; jj < SF_FIXED; ++jj) {
		_sf_indices[jj] = jj;
		_sf[jj] = RGBA_FILTERS[jj];
	}

	// Sort the best awards
	int count = MAX_FILTERS - SF_FIXED;
	FilterScorer::Score *top = awards.getTop(count, true);

	// Initialize coverage
	const int coverage_thresh = _tiles_x * _tiles_y;
	int coverage = 0;
	int sf_count = SF_FIXED;

	// Design remaining filter functions
	while (count-- ) {
		int index = top->index;
		int score = top->score;
		++top;

		// Accumulate coverage
		int covered = score / 5;
		coverage += covered;

		// If this filter is not already added,
		if (index >= SF_FIXED) {
			_sf_indices[sf_count] = index;
			_sf[sf_count] = RGBA_FILTERS[index];
			++sf_count;
		}

		// Stop when coverage achieved
		if (coverage >= coverage_thresh) {
			break;
		}
	}

	_sf_count = sf_count;
}

void ImageRGBAWriter::designTiles() {
	CAT_INANE("RGBA") << "Designing SF/CF tiles for " << _tiles_x << "x" << _tiles_y << "...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _size_x, size_y = _size_y;
	u8 FPT[3];

	EntropyEstimator ee[3];
	ee[0].init();
	ee[1].init();
	ee[2].init();

	// Allocate temporary space for entropy analysis
	const u32 code_stride = _tile_size_x * _tile_size_y;
	const u32 codes_size = code_stride * _sf_count * CF_COUNT;
	_ecodes[0].resize(codes_size);
	_ecodes[1].resize(codes_size);
	_ecodes[2].resize(codes_size);
	u8 *codes[3] = {
		_ecodes[0].get(),
		_ecodes[1].get(),
		_ecodes[2].get()
	};

	// Until revisits are done,
	int passes = 0;
	int revisitCount = _knobs->cm_revisitCount;
	u8 *sf = _sf_tiles.get();
	u8 *cf = _cf_tiles.get();
	while (passes < MAX_PASSES) {
		// For each tile,
		const u8 *topleft = _rgba;
		int ty = 0;
		for (u16 y = 0; y < size_y; y += tile_size_y, ++ty) {
			int tx = 0;
			for (u16 x = 0; x < size_x; x += tile_size_x, ++sf, ++cf, topleft += tile_size_x * 4, ++tx) {
				u8 osf = *sf;

				// If tile is masked,
				if (osf == MASK_TILE) {
					continue;
				}

				u8 ocf = *cf;

				// If we are on the second or later pass,
				if (passes > 0) {
					// If just finished revisiting old zones,
					if (--revisitCount < 0) {
						// Done!
						return;
					}

					int code_count = 0;

					// For each element in the tile,
					const u8 *row = topleft;
					u16 py = y, cy = tile_size_y;
					while (cy-- > 0 && py < size_y) {
						const u8 *data = row;
						u16 px = x, cx = tile_size_x;
						while (cx-- > 0 && px < size_x) {
							// If element is not masked,
							if (!IsMasked(px, py)) {
								const u8 *pred = _sf[osf].safe(data, FPT, px, py, size_x);
								u8 residual_rgb[3] = {
									data[0] - pred[0],
									data[1] - pred[1],
									data[2] - pred[2]
								};

								u8 yuv[3];
								RGB2YUV_FILTERS[ocf](residual_rgb, yuv);

								codes[0][code_count] = yuv[0];
								codes[1][code_count] = yuv[1];
								codes[2][code_count] = yuv[2];
								++code_count;
							}
							data += 4;
						}
						++py;
						row += size_x * 4;
					}

					ee[0].subtract(codes[0], code_count);
					ee[1].subtract(codes[1], code_count);
					ee[2].subtract(codes[2], code_count);
				}

				int code_count = 0;

				// For each element in the tile,
				const u8 *row = topleft;
				u16 py = y, cy = tile_size_y;
				while (cy-- > 0 && py < size_y) {
					const u8 *data = row;
					u16 px = x, cx = tile_size_x;
					while (cx-- > 0 && px < size_x) {
						// If element is not masked,
						if (!IsMasked(px, py)) {
							u8 *dest_y = codes[0] + code_count;
							u8 *dest_u = codes[1] + code_count;
							u8 *dest_v = codes[2] + code_count;

							// For each spatial filter,
							for (int sfi = 0, sfi_end = _sf_count; sfi < sfi_end; ++sfi) {
								const u8 *pred = _sf[sfi].safe(data, FPT, px, py, size_x);
								u8 residual_rgb[3] = {
									data[0] - pred[0],
									data[1] - pred[1],
									data[2] - pred[2]
								};

								// For each color filter,
								for (int cfi = 0; cfi < CF_COUNT; ++cfi) {
									u8 yuv[3];
									RGB2YUV_FILTERS[cfi](residual_rgb, yuv);

									*dest_y = yuv[0];
									*dest_u = yuv[1];
									*dest_v = yuv[2];
									dest_y += code_stride;
									dest_u += code_stride;
									dest_v += code_stride;
								}
							}

							++code_count;
						}
						++data;
					}
					++py;
					row += size_x;
				}

				// Evaluate entropy of codes
				u8 *src_y = codes[0];
				u8 *src_u = codes[1];
				u8 *src_v = codes[2];
				int lowest_entropy = 0x7fffffff;
				int best_sf = 0, best_cf = 0;

				for (int sfi = 0, sfi_end = _sf_count; sfi < sfi_end; ++sfi) {
					for (int cfi = 0; cfi < CF_COUNT; ++cfi) {
						int entropy = ee[0].entropy(src_y, code_count);
						entropy += ee[1].entropy(src_u, code_count);
						entropy += ee[2].entropy(src_v, code_count);

						if (lowest_entropy > entropy) {
							lowest_entropy = entropy;
							best_sf = sfi;
							best_cf = cfi;
						}

						src_y += code_stride;
						src_u += code_stride;
						src_v += code_stride;
					}
				}

				*sf = best_sf;
				*cf = best_cf;
			}
		}

		CAT_INANE("RGBA") << "Revisiting filter selections from the top... " << revisitCount << " left";
	}
}

bool ImageRGBAWriter::compressAlpha() {
	CAT_INANE("RGBA") << "Compressing alpha channel...";

	// Generate alpha matrix
	const int alpha_size = _size_x * _size_y;
	_alpha.resize(alpha_size);

	u8 *a = _alpha.get();
	const u8 *rgba = _rgba;
	for (int y = 0; y < _size_y; ++y) {
		for (int x = 0; x < _size_x; ++x) {
			*a++ = rgba[3];
			rgba += 4;
		}
	}

	MonoWriter::Parameters params;

	params.knobs = _knobs;
	params.data = _alpha.get();
	params.num_syms = 256;
	params.size_x = _size_x;
	params.size_y = _size_y;
	params.max_filters = 32;
	params.min_bits = 2;
	params.max_bits = 5;
	params.sympal_thresh = 0.9;
	params.filter_thresh = 0.9;
	params.mask.SetMember<ImageRGBAWriter, &ImageRGBAWriter::IsMasked>(this);
	params.AWARDS[0] = 5;
	params.AWARDS[1] = 3;
	params.AWARDS[2] = 1;
	params.AWARDS[3] = 1;
	params.award_count = 4;

	return _a_encoder.init(params);
}

void ImageRGBAWriter::computeResiduals() {
	CAT_INANE("RGBA") << "Executing tiles to generate residual matrix...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _size_x, size_y = _size_y;
	u8 FPT[3];

	const u8 *sf = _sf_tiles.get();
	const u8 *cf = _cf_tiles.get();

	_residuals.resize(_size_x * _size_y * 4);

	// For each tile,
	const u8 *topleft = _rgba;
	size_t residual_delta = (size_t)(_residuals.get() - topleft);
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		for (u16 x = 0; x < size_x; x += tile_size_x, ++sf, ++cf, topleft += tile_size_x*4) {
			const u8 sfi = *sf;

			if (sfi == MASK_TILE) {
				continue;
			}

			const u8 cfi = *cf;

			// For each element in the tile,
			const u8 *row = topleft;
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				const u8 *data = row;
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If element is not masked,
					if (!IsMasked(px, py)) {
						const u8 *pred = _sf[sfi].safe(data, FPT, px, py, size_x);
						u8 residual_rgb[3] = {
							data[0] - pred[0],
							data[1] - pred[1],
							data[2] - pred[2]
						};

						u8 yuv[3];
						RGB2YUV_FILTERS[cfi](residual_rgb, yuv);

						u8 *residual_data = (u8*)data + residual_delta;

						residual_data[0] = yuv[0];
						residual_data[1] = yuv[1];
						residual_data[2] = yuv[2];
					}
					data += 4;
				}
				++py;
				row += size_x*4;
			}
		}
	}
}

void ImageRGBAWriter::designChaos() {
	CAT_INANE("RGBA") << "Designing chaos...";

	EntropyEstimator ee[MAX_CHAOS_LEVELS];

	u32 best_entropy = 0x7fffffff;
	int best_chaos_levels = 1;

	// For each chaos level,
	for (int chaos_levels = 1; chaos_levels < MAX_CHAOS_LEVELS; ++chaos_levels) {
		_chaos.init(chaos_levels, _size_x);

		// Reset entropy estimator
		for (int ii = 0; ii < chaos_levels; ++ii) {
			ee[ii].init();
		}

		_chaos.start();

		// For each row,
		const u8 *residuals = _residuals.get();
		for (int y = 0; y < _size_y; ++y) {
			_chaos.startRow();

			// For each column,
			for (int x = 0; x < _size_x; ++x) {
				// If masked,
				if (IsMasked(x, y)) {
					_chaos.zero();
				} else {
					// Get chaos bin
					const u8 chaos_y = _chaos.getChaosY();
					const u8 chaos_u = _chaos.getChaosU();
					const u8 chaos_v = _chaos.getChaosV();

					// Update chaos
					_chaos.store(residuals[0], residuals[1], residuals[2], 0);

					// Add to histogram for this chaos bin
					ee[chaos_y].addSingle(residuals[0]);
					ee[chaos_u].addSingle(residuals[1]);
					ee[chaos_v].addSingle(residuals[2]);
				}

				residuals += 4;
			}
		}

		// For each chaos level,
		u32 entropy = 0;
		for (int ii = 0; ii < chaos_levels; ++ii) {
			entropy += ee[ii].entropyOverall();

			// Approximate cost of adding an entropy level
			entropy += 3 * 5 * 256;
		}

		// If this is the best chaos levels so far,
		if (best_entropy > entropy) {
			best_entropy = entropy;
			best_chaos_levels = chaos_levels;
		}
	}

	// Record the best option found
	_chaos.init(best_chaos_levels, _size_x);
}

bool ImageRGBAWriter::compressSF() {
	MonoWriter::Parameters params;

	params.knobs = _knobs;
	params.data = _sf_tiles.get();
	params.num_syms = _sf_count;
	params.size_x = _tiles_x;
	params.size_y = _tiles_y;
	params.max_filters = 32;
	params.min_bits = 2;
	params.max_bits = 5;
	params.sympal_thresh = 0.9;
	params.filter_thresh = 0.9;
	params.mask.SetMember<ImageRGBAWriter, &ImageRGBAWriter::IsMasked>(this);
	params.AWARDS[0] = 5;
	params.AWARDS[1] = 3;
	params.AWARDS[2] = 1;
	params.AWARDS[3] = 1;
	params.award_count = 4;

	return _sf_encoder.init(params);
}

bool ImageRGBAWriter::compressCF() {
	MonoWriter::Parameters params;

	params.knobs = _knobs;
	params.data = _cf_tiles.get();
	params.num_syms = CF_COUNT;
	params.size_x = _tiles_x;
	params.size_y = _tiles_y;
	params.max_filters = 32;
	params.min_bits = 2;
	params.max_bits = 5;
	params.sympal_thresh = 0.9;
	params.filter_thresh = 0.9;
	params.mask.SetMember<ImageRGBAWriter, &ImageRGBAWriter::IsMasked>(this);
	params.AWARDS[0] = 5;
	params.AWARDS[1] = 3;
	params.AWARDS[2] = 1;
	params.AWARDS[3] = 1;
	params.award_count = 4;

	return _cf_encoder.init(params);
}

void ImageRGBAWriter::initializeEncoders() {
	_chaos.start();
	int chaos_count = 0;

	// For each row,
	const u8 *residuals = _residuals.get();
	for (int y = 0; y < _size_y; ++y) {
		_chaos.startRow();

		// For each column,
		for (int x = 0; x < _size_x; ++x) {
			// If masked,
			if (IsMasked(x, y)) {
				_chaos.zero();
			} else {
				// Get chaos bin
				const u8 chaos_y = _chaos.getChaosY();
				const u8 chaos_u = _chaos.getChaosU();
				const u8 chaos_v = _chaos.getChaosV();

				// Update chaos
				_chaos.store(residuals[0], residuals[1], residuals[2], 0);

				// Add to histogram for this chaos bin
				_y_encoder[chaos_y].add(residuals[0]);
				_u_encoder[chaos_u].add(residuals[1]);
				_v_encoder[chaos_v].add(residuals[2]);

				++chaos_count;
			}

			residuals += 4;
		}
	}

#ifdef CAT_COLLECT_STATS
	Stats.chaos_count = chaos_count;
	Stats.chaos_bins = _chaos.getBinCount();
#endif

	// For each chaos level,
	for (int ii = 0, iiend = _chaos.getBinCount(); ii < iiend; ++ii) {
		_y_encoder[ii].finalize();
		_u_encoder[ii].finalize();
		_v_encoder[ii].finalize();
	}
}

bool ImageRGBAWriter::IsMasked(u16 x, u16 y) {
	return _mask->masked(x, y) && _lz->visited(x, y);
}

int ImageRGBAWriter::init(const u8 *rgba, int size_x, int size_y, ImageMaskWriter &mask, ImageLZWriter &lz, const GCIFKnobs *knobs) {
	_knobs = knobs;
	_rgba = rgba;
	_mask = &mask;
	_lz = &lz;

	if (size_x < 0 || size_y < 0) {
		return GCIF_WE_BAD_DIMS;
	}

	if ((!knobs->cm_disableEntropy && knobs->cm_filterSelectFuzz <= 0)) {
		return GCIF_WE_BAD_PARAMS;
	}

	_size_x = size_x;
	_size_y = size_y;

	// Use constant tile size of 4x4 for now
	_tile_bits_x = 2;
	_tile_bits_y = 2;
	_tile_size_x = 1 << _tile_bits_x;
	_tile_size_y = 1 << _tile_bits_y;
	_tiles_x = (_size_x + _tile_size_x - 1) >> _tile_bits_x;
	_tiles_y = (_size_y + _tile_size_y - 1) >> _tile_bits_y;

	const int tiles_size = _tiles_x * _tiles_y;
	_sf_tiles.resize(tiles_size);
	_cf_tiles.resize(tiles_size);

	maskTiles();
	designFilters();
	designTiles();
	computeResiduals();
	compressAlpha();
	designChaos();
	compressSF();
	compressCF();
	initializeEncoders();

	return GCIF_WE_OK;
}

bool ImageRGBAWriter::writeTables(ImageWriter &writer) {
	CAT_DEBUG_ENFORCE(MAX_FILTERS <= 32);
	CAT_DEBUG_ENFORCE(SF_COUNT <= 128);

	writer.writeBits(_tile_bits_x, 3);
	int basic_bits = 3;

	DESYNC_TABLE();

	// Write filter choices
	writer.writeBits(_sf_count - SF_FIXED, 5);
	int choice_bits = 5;

	for (int ii = SF_FIXED; ii < _sf_count; ++ii) {
		u16 sf = _sf_indices[ii];

		writer.writeBits(sf, 7);
		choice_bits += 7;
	}

	DESYNC_TABLE();

	int sf_table_bits = _sf_encoder.writeTables(writer);

	DESYNC_TABLE();

	int cf_table_bits = _cf_encoder.writeTables(writer);

	DESYNC_TABLE();

	int af_table_bits = _af_encoder.writeTables(writer);

	DESYNC_TABLE();

#ifdef CAT_COLLECT_STATS
	Stats.y_table_bits = 0;
	Stats.u_table_bits = 0;
	Stats.v_table_bits = 0;
#endif // CAT_COLLECT_STATS

	writer.writeBits(_chaos.getBinCount() - 1, 4);
	basic_bits += 4;

	for (int jj = 0; jj < _chaos.getBinCount(); ++jj) {
		int y_table_bits = _y_encoder[jj].writeTables(writer);
		DESYNC_TABLE();
		int u_table_bits = _u_encoder[jj].writeTables(writer);
		DESYNC_TABLE();
		int v_table_bits = _v_encoder[jj].writeTables(writer);
		DESYNC_TABLE();

#ifdef CAT_COLLECT_STATS
		Stats.y_table_bits += y_table_bits;
		Stats.u_table_bits += u_table_bits;
		Stats.v_table_bits += v_table_bits;
#endif // CAT_COLLECT_STATS
	}

#ifdef CAT_COLLECT_STATS
	Stats.basic_overhead_bits = basic_bits;
	Stats.sf_choice_bits = choice_bits;
	Stats.sf_table_bits = sf_table_bits;
	Stats.cf_table_bits = cf_table_bits;
	Stats.af_table_bits = af_table_bits;
#endif // CAT_COLLECT_STATS

	return true;
}

bool ImageRGBAWriter::writePixels(ImageWriter &writer) {
#ifdef CAT_COLLECT_STATS
	int sf_bits = 0, cf_bits = 0, y_bits = 0, u_bits = 0, v_bits = 0, a_bits = 0;
#endif

	_seen_filter.resize(_tiles_x);

	_chaos.start();

	// For each scanline,
	const u8 *residuals = _residuals.get();
	const u16 tile_mask = _tile_size_y - 1;
	for (u16 y = 0; y < _size_y; ++y) {
		_chaos.startRow();

		// If at the start of a tile row,
		if ((y & tile_mask) == 0) {
			_seen_filter.fill_00();

			_sf_encoder.writeRowHeader(y, writer);
			_cf_encoder.writeRowHeader(y, writer);
			_a_encoder.writeRowHeader(y, writer);
		}

		// For each pixel,
		for (u16 x = 0; x < _size_x; ++x) {
			DESYNC(x, y);

			// If masked,
			if (IsMasked(x, y)) {
				_chaos.zero();
			} else {
				// Get chaos bin
				const u8 chaos_y = _chaos.getChaosY();
				const u8 chaos_u = _chaos.getChaosU();
				const u8 chaos_v = _chaos.getChaosV();

				// Update chaos
				_chaos.store(residuals[0], residuals[1], residuals[2], 0);

				// Add to histogram for this chaos bin
				y_bits += _y_encoder[chaos_y].add(residuals[0]);
				u_bits += _u_encoder[chaos_u].add(residuals[1]);
				v_bits += _v_encoder[chaos_v].add(residuals[2]);
				a_bits += _a_encoder.write(x, y, writer);
			}

			residuals += 4;
		}
	}

#ifdef CAT_COLLECT_STATS
	Stats.sf_bits = sf_bits;
	Stats.cf_bits = cf_bits;
	Stats.y_bits = y_bits;
	Stats.u_bits = u_bits;
	Stats.v_bits = v_bits;
	Stats.a_bits = a_bits;
#endif

	return true;
}

void ImageRGBAWriter::write(ImageWriter &writer) {
	CAT_INANE("RGBA") << "Writing encoded pixel data...";

	writeTables(writer);
	writePixels(writer);

#ifdef CAT_COLLECT_STATS
	int total = 0;
	for (int ii = 0; ii < 2; ++ii) {
		total += Stats.filter_table_bits[ii];
		total += Stats.filter_compressed_bits[ii];
	}
	for (int ii = 0; ii < 4; ++ii) {
		total += Stats.rgba_bits[ii];
	}
	total += Stats.chaos_overhead_bits;
	Stats.chaos_bits = total;
	total += _lz->Stats.huff_bits;
	total += _mask->Stats.compressedDataBits;
	Stats.total_bits = total;

	Stats.overall_compression_ratio = _size_x * _size_y * 4 * 8 / (double)Stats.total_bits;

	Stats.chaos_compression_ratio = Stats.chaos_count * 4 * 8 / (double)Stats.chaos_bits;
#endif
}

#ifdef CAT_COLLECT_STATS

bool ImageRGBAWriter::dumpStats() {
	CAT_INANE("stats") << "(RGBA Compress) Spatial Filter Table Size : " <<  Stats.filter_table_bits[0] << " bits (" << Stats.filter_table_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) Spatial Filter Compressed Size : " <<  Stats.filter_compressed_bits[0] << " bits (" << Stats.filter_compressed_bits[0]/8 << " bytes)";

	CAT_INANE("stats") << "(RGBA Compress) Color Filter Table Size : " <<  Stats.filter_table_bits[1] << " bits (" << Stats.filter_table_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) Color Filter Compressed Size : " <<  Stats.filter_compressed_bits[1] << " bits (" << Stats.filter_compressed_bits[1]/8 << " bytes)";

	CAT_INANE("stats") << "(RGBA Compress) Y-Channel Compressed Size : " <<  Stats.rgb_bits[0] << " bits (" << Stats.rgb_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) U-Channel Compressed Size : " <<  Stats.rgb_bits[1] << " bits (" << Stats.rgb_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) V-Channel Compressed Size : " <<  Stats.rgb_bits[2] << " bits (" << Stats.rgb_bits[2]/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) A-Channel Compressed Size : " <<  Stats.rgb_bits[3] << " bits (" << Stats.rgb_bits[3]/8 << " bytes)";

	CAT_INANE("stats") << "(RGBA Compress) YUVA Overhead Size : " << Stats.chaos_overhead_bits << " bits (" << Stats.chaos_overhead_bits/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) Chaos pixel count : " << Stats.chaos_count << " pixels";
	CAT_INANE("stats") << "(RGBA Compress) Chaos compression ratio : " << Stats.chaos_compression_ratio << ":1";
	CAT_INANE("stats") << "(RGBA Compress) Overall size : " << Stats.total_bits << " bits (" << Stats.total_bits/8 << " bytes)";
	CAT_INANE("stats") << "(RGBA Compress) Overall compression ratio : " << Stats.overall_compression_ratio << ":1";
	CAT_INANE("stats") << "(RGBA Compress) Image dimensions were : " << _size_x << " x " << _size_y << " pixels";

	return true;
}

#endif

