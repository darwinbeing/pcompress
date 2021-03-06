/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *      
 */

/*
   xxHash - Fast Hash algorithm
   Copyright (C) 2012, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

	You can contact the author at :
	- xxHash source repository : http://code.google.com/p/xxhash/
*/

/*
 * Modified by Moinak Ghosh for pcompress. The new hashing approach
 * with interleaved blocks is derived from the following paper:
 * 
 * http://eprint.iacr.org/2012/476.pdf
 */

//**************************************
// Tuning parameters
//**************************************
// FORCE_NATIVE_FORMAT :
// By default, xxHash library provides endian-independant Hash values.
// Results are therefore identical for big-endian and little-endian CPU.
// This comes at a  performance cost for big-endian CPU, since some swapping is required to emulate little-endian format.
// Should endian-independance be of no importance to your application, you may uncomment the #define below
// It will improve speed for Big-endian CPU.
// This option has no impact on Little_Endian CPU.
//#define FORCE_NATIVE_FORMAT 1



//**************************************
// Includes
//**************************************
#include <stdlib.h>    // for malloc(), free()
#include <string.h>    // for memcpy()
#include "xxhash.h"



//**************************************
// CPU Feature Detection
//**************************************
// Little Endian or Big Endian ?
// You can overwrite the #define below if you know your architecture endianess
#if defined(FORCE_NATIVE_FORMAT) && (FORCE_NATIVE_FORMAT==1)
// Force native format. The result will be endian dependant.
#  define XXH_BIG_ENDIAN 0
#elif defined (__GLIBC__)
#  include <endian.h>
#  if (__BYTE_ORDER == __BIG_ENDIAN)
#     define XXH_BIG_ENDIAN 1
#  endif
#elif (defined(__BIG_ENDIAN__) || defined(__BIG_ENDIAN) || defined(_BIG_ENDIAN)) && !(defined(__LITTLE_ENDIAN__) || defined(__LITTLE_ENDIAN) || defined(_LITTLE_ENDIAN))
#  define XXH_BIG_ENDIAN 1
#elif defined(__sparc) || defined(__sparc__) \
   || defined(__ppc__) || defined(_POWER) || defined(__powerpc__) || defined(_ARCH_PPC) || defined(__PPC__) || defined(__PPC) || defined(PPC) || defined(__powerpc__) || defined(__powerpc) || defined(powerpc) \
   || defined(__hpux)  || defined(__hppa) \
   || defined(_MIPSEB) || defined(__s390__)
#  define XXH_BIG_ENDIAN 1
#endif

#if !defined(XXH_BIG_ENDIAN)
// Little Endian assumed. PDP Endian and other very rare endian format are unsupported.
#  define XXH_BIG_ENDIAN 0
#endif

//**************************************
// Compiler-specific Options & Functions
//**************************************
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

// Note : under GCC, it may sometimes be faster to enable the (2nd) macro definition, instead of using win32 intrinsic
#if defined(_WIN32)
#  define XXH_rotl32(x,r) _rotl(x,r)
#else
#  define XXH_rotl32(x,r) ((x << r) | (x >> (32 - r)))
#endif

#if defined(_MSC_VER)     // Visual Studio
#  define XXH_swap32 _byteswap_ulong
#elif GCC_VERSION >= 403
#  define XXH_swap32 __builtin_bswap32
#else
static inline unsigned int XXH_swap32 (unsigned int x) {
                        return  ((x << 24) & 0xff000000 ) |
                                ((x <<  8) & 0x00ff0000 ) |
                                ((x >>  8) & 0x0000ff00 ) |
                                ((x >> 24) & 0x000000ff );
                 }
#endif

#if defined(__USE_SSE_INTRIN__) && defined(__SSE4_1__)
#include <smmintrin.h>
static inline __m128i _x_mm_rotl_epi32(const __m128i a, int bits)
{
	__m128i tmp1 = _mm_slli_epi32(a, bits);
	__m128i tmp2 = _mm_srli_epi32(a, 32 - bits);
	return (_mm_or_si128(tmp1, tmp2));
}
#endif

//**************************************
// Constants
//**************************************
#define PRIME32_1   2654435761U
#define PRIME32_2   2246822519U
#define PRIME32_3   3266489917U
#define PRIME32_4    668265263U
#define PRIME32_5    374761393U



//**************************************
// Macros
//**************************************
#define XXH_LE32(p)  (XXH_BIG_ENDIAN ? XXH_swap32(*(unsigned int*)(p)) : *(unsigned int*)(p))



//****************************
// Simple Hash Functions
//****************************

unsigned int CPUCAP_NM(XXH32)(const void* input, int len, unsigned int seed)
{
#if 0
	// Simple version, good for code maintenance, but unfortunately slow for small inputs
	void* state = XXH32_init(seed);
	XXH32_feed(state, input, len);
	return XXH32_result(state);
#else

	const unsigned char* p = (const unsigned char*)input;
	const unsigned char* p1 = p;
	const unsigned char* const bEnd = p + len;
	unsigned int h32;

	if (len>=32)
	{
		const unsigned char* const limit = bEnd - 32;
		unsigned int v1 = seed + PRIME32_1 + PRIME32_2;
		unsigned int v2 = seed + PRIME32_2;
		unsigned int v3 = seed + 0;
		unsigned int v4 = seed - PRIME32_1;
#if defined(__USE_SSE_INTRIN__) && defined(__SSE4_1__)
		unsigned int vx[4], vx1[4];

		__m128i accum = _mm_set_epi32(v4, v3, v2, v1);
		__m128i accum1 = _mm_set_epi32(v4, v3, v2, v1);
		__m128i prime1 = _mm_set1_epi32(PRIME32_1);
		__m128i prime2 = _mm_set1_epi32(PRIME32_2);

		/*
		 * 4-way SIMD calculations with 4 ints in two blocks for 2 accumulators will
		 * interleave to some extent on a superscalar processor providing 10% - 14%
		 * speedup over original xxhash depending on processor. We could have used
		 * aligned loads but we actually want the unaligned penalty. It helps to
		 * interleave better for a slight benefit over aligned loads here!
		 */
		do {
			__m128i mem = _mm_loadu_si128((__m128i *)p);
			p += 16;
			mem = _mm_mullo_epi32(mem, prime2);
			accum = _mm_add_epi32(accum, mem);
			accum = _x_mm_rotl_epi32(accum, 13);
			accum = _mm_mullo_epi32(accum, prime1);

			mem = _mm_loadu_si128((__m128i *)p);
			p += 16;
			mem = _mm_mullo_epi32(mem, prime2);
			accum1 = _mm_add_epi32(accum1, mem);
			accum1 = _x_mm_rotl_epi32(accum1, 13);
			accum1 = _mm_mullo_epi32(accum1, prime1);
		} while (p<=limit);

		_mm_storeu_si128((__m128i *)vx, accum);
		_mm_storeu_si128((__m128i *)vx1, accum1);

		/*
		 * Combine the two accumulators into a single hash value.
		 */
		v1 = vx[0];
		v2 = vx[1];
		v3 = vx[2];
		v4 = vx[3];
		v1 += vx1[0] * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1;
		v2 += vx1[1] * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1;
		v3 += vx1[2] * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1;
		v4 += vx1[3] * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1;
		h32 = XXH_rotl32(v1, 1) + XXH_rotl32(v2, 7) + XXH_rotl32(v3, 12) + XXH_rotl32(v4, 18);
#else
		unsigned int vx1 = seed + PRIME32_1 + PRIME32_2;
		unsigned int vx2 = seed + PRIME32_2;
		unsigned int vx3 = seed + 0;
		unsigned int vx4 = seed - PRIME32_1;

		do
		{
			v1 += XXH_LE32(p) * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1; p+=4;
			v2 += XXH_LE32(p) * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1; p+=4;
			v3 += XXH_LE32(p) * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1; p+=4;
			v4 += XXH_LE32(p) * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1; p+=4;

			vx1 += XXH_LE32(p) * PRIME32_2; vx1 = XXH_rotl32(vx1, 13); vx1 *= PRIME32_1; p+=4;
			vx2 += XXH_LE32(p) * PRIME32_2; vx2 = XXH_rotl32(vx2, 13); vx2 *= PRIME32_1; p+=4;
			vx3 += XXH_LE32(p) * PRIME32_2; vx3 = XXH_rotl32(vx3, 13); vx3 *= PRIME32_1; p+=4;
			vx4 += XXH_LE32(p) * PRIME32_2; vx4 = XXH_rotl32(vx4, 13); vx4 *= PRIME32_1; p+=4;
		} while (p<=limit) ;
		v1 += vx1 * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1;
		v2 += vx2 * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1;
		v3 += vx3 * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1;
		v4 += vx4 * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1;
		h32 = XXH_rotl32(v1, 1) + XXH_rotl32(v2, 7) + XXH_rotl32(v3, 12) + XXH_rotl32(v4, 18);
#endif
		len = p - p1;
	}
	else
	{
		h32  = seed + PRIME32_5;
	}

	h32 += (unsigned int) len;
	
	while (p<=bEnd-4)
	{
		h32 += XXH_LE32(p) * PRIME32_3;
		h32 = XXH_rotl32(h32, 17) * PRIME32_4 ;
		p+=4;
	}

	while (p<bEnd)
	{
		h32 += (*p) * PRIME32_5;
		h32 = XXH_rotl32(h32, 11) * PRIME32_1 ;
		p++;
	}

	h32 ^= h32 >> 15;
	h32 *= PRIME32_2;
	h32 ^= h32 >> 13;
	h32 *= PRIME32_3;
	h32 ^= h32 >> 16;

	return h32;

#endif
}


//****************************
// Advanced Hash Functions
//****************************

struct XXH_state32_t
{
	unsigned int seed;
	unsigned int v1, vx1;
	unsigned int v2, vx2;
	unsigned int v3, vx3;
	unsigned int v4, vx4;
	unsigned long long total_len;
	char memory[32];
	int memsize;
};


void* CPUCAP_NM(XXH32_init) (unsigned int seed)
{
	struct XXH_state32_t * state = (struct XXH_state32_t *) malloc ( sizeof(struct XXH_state32_t));
	state->seed = seed;
	state->v1 = seed + PRIME32_1 + PRIME32_2;
	state->v2 = seed + PRIME32_2;
	state->v3 = seed + 0;
	state->v4 = seed - PRIME32_1;
	state->vx1 = seed + PRIME32_1 + PRIME32_2;
	state->vx2 = seed + PRIME32_2;
	state->vx3 = seed + 0;
	state->vx4 = seed - PRIME32_1;
	state->total_len = 0;
	state->memsize = 0;

	return (void*)state;
}


int   CPUCAP_NM(XXH32_feed) (void* state_in, const void* input, int len)
{
	struct XXH_state32_t * state = state_in;
	const unsigned char* p = (const unsigned char*)input;
	const unsigned char* const bEnd = p + len;

	state->total_len += len;
	
	if (state->memsize + len < 32)   // fill in tmp buffer
	{
		memcpy(state->memory + state->memsize, input, len);
		state->memsize +=  len;
		return 0;
	}

	if (state->memsize)   // some data left from previous feed
	{
		memcpy(state->memory + state->memsize, input, 32-state->memsize);
		{
			const unsigned int* p32 = (const unsigned int*)state->memory;
			state->v1 += XXH_LE32(p32) * PRIME32_2; state->v1 = XXH_rotl32(state->v1, 13); state->v1 *= PRIME32_1;
			p32++;
			state->v2 += XXH_LE32(p32) * PRIME32_2; state->v2 = XXH_rotl32(state->v2, 13); state->v2 *= PRIME32_1;
			p32++; 
			state->v3 += XXH_LE32(p32) * PRIME32_2; state->v3 = XXH_rotl32(state->v3, 13); state->v3 *= PRIME32_1;
			p32++;
			state->v4 += XXH_LE32(p32) * PRIME32_2; state->v4 = XXH_rotl32(state->v4, 13); state->v4 *= PRIME32_1;
			p32++;
			state->vx1 += XXH_LE32(p32) * PRIME32_2; state->vx1 = XXH_rotl32(state->vx1, 13); state->vx1 *= PRIME32_1;
			p32++;
			state->vx2 += XXH_LE32(p32) * PRIME32_2; state->vx2 = XXH_rotl32(state->vx2, 13); state->vx2 *= PRIME32_1;
			p32++; 
			state->vx3 += XXH_LE32(p32) * PRIME32_2; state->vx3 = XXH_rotl32(state->vx3, 13); state->vx3 *= PRIME32_1;
			p32++;
			state->vx4 += XXH_LE32(p32) * PRIME32_2; state->vx4 = XXH_rotl32(state->vx4, 13); state->vx4 *= PRIME32_1;
			p32++;
		}
		p += 32-state->memsize;
		len -= 32-state->memsize;
		state->memsize = 0;
	}

	if (len>=32)
	{
		const unsigned char* const limit = bEnd - 32;
#if defined(__USE_SSE_INTRIN__) && defined(__SSE4_1__)
		unsigned int vx[4], vx1[4];

		__m128i accum = _mm_set_epi32(state->v4, state->v3, state->v2, state->v1);
		__m128i accum1 = _mm_set_epi32(state->v4, state->v3, state->v2, state->v1);
		__m128i prime1 = _mm_set1_epi32(PRIME32_1);
		__m128i prime2 = _mm_set1_epi32(PRIME32_2);

		/*
		 * 4-way SIMD calculations with 4 ints in two blocks for 2 accumulators will
		 * interleave to some extent on the superscalar x86 processor providing
		 * 10% - 14% speedup over original xxhash depending on processor model. We
		 * could have used aligned loads but we actually want the unaligned penalty.
		 * It helps to interleave better for a slight benefit over aligned loads here!
		 */
		do {
			__m128i mem = _mm_loadu_si128((__m128i *)p);
			p += 16;
			mem = _mm_mullo_epi32(mem, prime2);
			accum = _mm_add_epi32(accum, mem);
			accum = _x_mm_rotl_epi32(accum, 13);
			accum = _mm_mullo_epi32(accum, prime1);

			mem = _mm_loadu_si128((__m128i *)p);
			p += 16;
			mem = _mm_mullo_epi32(mem, prime2);
			accum1 = _mm_add_epi32(accum1, mem);
			accum1 = _x_mm_rotl_epi32(accum1, 13);
			accum1 = _mm_mullo_epi32(accum1, prime1);
		} while (p<=limit);

		_mm_storeu_si128((__m128i *)vx, accum);
		_mm_storeu_si128((__m128i *)vx1, accum1);

		/*
		 * Combine the two accumulators into a single hash value.
		 */
		state->v1 = vx[0];
		state->v2 = vx[1];
		state->v3 = vx[2];
		state->v4 = vx[3];
		state->vx1 = vx1[0];
		state->vx2 = vx1[1];
		state->vx3 = vx1[2];
		state->vx4 = vx1[3];
#else
		unsigned int v1 = state->v1;
		unsigned int v2 = state->v2;
		unsigned int v3 = state->v3;
		unsigned int v4 = state->v4;
		unsigned int vx1 = state->vx1;
		unsigned int vx2 = state->vx2;
		unsigned int vx3 = state->vx3;
		unsigned int vx4 = state->vx4;

		do
		{
			v1 += XXH_LE32(p) * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1; p+=4;
			v2 += XXH_LE32(p) * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1; p+=4;
			v3 += XXH_LE32(p) * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1; p+=4;
			v4 += XXH_LE32(p) * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1; p+=4;

			vx1 += XXH_LE32(p) * PRIME32_2; vx1 = XXH_rotl32(vx1, 13); vx1 *= PRIME32_1; p+=4;
			vx2 += XXH_LE32(p) * PRIME32_2; vx2 = XXH_rotl32(vx2, 13); vx2 *= PRIME32_1; p+=4;
			vx3 += XXH_LE32(p) * PRIME32_2; vx3 = XXH_rotl32(vx3, 13); vx3 *= PRIME32_1; p+=4;
			vx4 += XXH_LE32(p) * PRIME32_2; vx4 = XXH_rotl32(vx4, 13); vx4 *= PRIME32_1; p+=4;
		} while (p<=limit) ;

		state->v1 = v1;
		state->v2 = v2;
		state->v3 = v3;
		state->v4 = v4;
		state->vx1 = vx1;
		state->vx2 = vx2;
		state->vx3 = vx3;
		state->vx4 = vx4;
#endif
	}

	if (p < bEnd)
	{
		memcpy(state->memory, p, bEnd-p);
		state->memsize = bEnd-p;
	}

	return 0;
}


unsigned int CPUCAP_NM(XXH32_getIntermediateResult) (void* state_in)
{
	struct XXH_state32_t * state = state_in;
	unsigned char * p   = (unsigned char*)state->memory;
	unsigned char* bEnd = (unsigned char*)state->memory + state->memsize;
	unsigned int h32;


	if (state->total_len >= 32)
	{
		unsigned int v1 = state->v1;
		unsigned int v2 = state->v2;
		unsigned int v3 = state->v3;
		unsigned int v4 = state->v4;

		v1 += state->vx1 * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1;
		v2 += state->vx2 * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1;
		v3 += state->vx3 * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1;
		v4 += state->vx4 * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1;
		h32 = XXH_rotl32(v1, 1) + XXH_rotl32(v2, 7) + XXH_rotl32(v3, 12) + XXH_rotl32(v4, 18);
	}
	else
	{
		h32  = state->seed + PRIME32_5;
	}

	h32 += (unsigned int) state->total_len;
	
	while (p<=bEnd-4)
	{
		h32 += XXH_LE32(p) * PRIME32_3;
		h32 = XXH_rotl32(h32, 17) * PRIME32_4 ;
		p+=4;
	}

	while (p<bEnd)
	{
		h32 += (*p) * PRIME32_5;
		h32 = XXH_rotl32(h32, 11) * PRIME32_1 ;
		p++;
	}

	h32 ^= h32 >> 15;
	h32 *= PRIME32_2;
	h32 ^= h32 >> 13;
	h32 *= PRIME32_3;
	h32 ^= h32 >> 16;

	return h32;
}


unsigned int CPUCAP_NM(XXH32_result) (void* state_in)
{
    unsigned int h32 = CPUCAP_NM(XXH32_getIntermediateResult)(state_in);

	free(state_in);

	return h32;
}
