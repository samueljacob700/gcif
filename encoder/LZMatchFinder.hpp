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

#ifndef LZ_MATCH_FINDER_HPP
#define LZ_MATCH_FINDER_HPP

#include "../decoder/Platform.hpp"
#include "../decoder/SmartArray.hpp"

/*
 * LZ Match Finder
 *
 * This LZ system is only designed for RGBA data at this time.
 */

namespace cat {


//// LZMatchFinder

class LZMatchFinder {
	static const int HASH_BITS = 18;
	static const u64 HASH_MULT = 0xc6a4a7935bd1e995ULL;

	// Returns hash for provided pixel and the following one
	static CAT_INLINE u32 HashPixels(const u32 * CAT_RESTRICT rgba) {
		(u32)( ( ((u64)rgba[0] << 32) | rgba[1] ) * HASH_MULT >> (64 - HASH_BITS) );
	}

	SmartArray<u32> _table;
	SmartArray<u16> _chain;
	int _pixels;

public:
	void scanRGBA(const u32 * CAT_RESTRICT rgba, int pixels);
};


} // namespace cat

#endif // LZ_MATCH_FINDER_HPP

