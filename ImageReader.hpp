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

#ifndef IMAGE_READER_HPP
#define IMAGE_READER_HPP

#include "Platform.hpp"
#include "HotRodHash.hpp"
#include "MappedFile.hpp"
#include "Log.hpp"

namespace cat {


struct ImageHeader {
	u16 width, height; // pixels

	u32 headHash;	// Hash of head words
	u32 fastHash;	// Fast hash of data words (used during normal decoding)
	u32 goodHash;	// Good hash of data words
};


//// ImageReader

class ImageReader {
	MappedFile _file;
	MappedView _fileView;

	ImageHeader _header;

	HotRodHash _hash;

	bool _eof;

	const u32 *_words;
	int _wordCount;
	int _wordsLeft;

	u64 _bits;
	int _bitsLeft;

	void clear();

	u32 refill();

public:
	ImageReader() {
		_words = 0;
	}
	virtual ~ImageReader() {
	}

	CAT_INLINE int getTotalDataWords() {
		return _wordCount;
	}

	CAT_INLINE int getWordsLeft() {
		return _wordsLeft;
	}

	// Initialize with file or memory buffer
	int init(const char *path);
	int init(const void *buffer, int bytes);

	CAT_INLINE ImageHeader *getImageHeader() {
		return &_header;
	}

	// Returns at least minBits in the high bits, supporting up to 32 bits
	CAT_INLINE u32 peek(int minBits) {
		if CAT_UNLIKELY(_bitsLeft < minBits) {
			return refill();
		} else {
			return (u32)(_bits >> 32);
		}
	}

	// After peeking, consume up to 31 bits
	CAT_INLINE void eat(int len) {
		CAT_DEBUG_ENFORCE(len <= 31);

		_bits <<= len;
		_bitsLeft -= len;
	}

	// Read up to 31 bits
	CAT_INLINE u32 readBits(int len) {
		CAT_DEBUG_ENFORCE(len >= 1 && len <= 31);

		const u32 bits = peek(len);
		eat(len);
		return bits >> (32 - len);
	}

	// Read one bit
	CAT_INLINE u32 readBit() {
		return readBits(1);
	}

	// Read 32 bits
	CAT_INLINE u32 readWord() {
		const u32 bits = peek(32);
		_bitsLeft = 0;
		_bits = 0;
		return bits;
	}

	// No bits left to read?
	CAT_INLINE bool eof() {
		return _eof;
	}

	static const u32 HEAD_WORDS = 5;
	static const u32 HEAD_MAGIC = 0x46494347;
	static const u32 HEAD_SEED = 0x120CA71D;
	static const u32 DATA_SEED = 0xCA71D123;

	CAT_INLINE bool finalizeCheckHash() {
		return _header.fastHash == _hash.final(_wordCount);
	}
};

} // namespace cat

#endif // IMAGE_READER_HPP

