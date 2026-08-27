// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Overdrive (from plaits/dsp/fx/overdrive.h) - used after the 808 bass drum.

#pragma once

#include "dsp/drums/stmlib.h"
#include <cstddef>

namespace deluge::dsp::drums {

class Overdrive {
public:
	void Init() {
		pre_gain_ = 0.0f;
		post_gain_ = 0.0f;
	}

	void Process(float drive, float* in_out, size_t size) {
		const float drive_2 = drive * drive;
		const float pre_gain_a = drive * 0.5f;
		const float pre_gain_b = drive_2 * drive_2 * drive * 24.0f;
		const float pre_gain = pre_gain_a + (pre_gain_b - pre_gain_a) * drive_2;
		const float drive_squashed = drive * (2.0f - drive);
		const float post_gain = 1.0f / SoftClip(0.33f + drive_squashed * (pre_gain - 0.33f));

		ParameterInterpolator pre_gain_modulation(&pre_gain_, pre_gain, size);
		ParameterInterpolator post_gain_modulation(&post_gain_, post_gain, size);

		while (size--) {
			float pre = pre_gain_modulation.Next() * *in_out;
			*in_out++ = SoftClip(pre) * post_gain_modulation.Next();
		}
	}

private:
	float pre_gain_;
	float post_gain_;
};

} // namespace deluge::dsp::drums
