# The MIT License
#
# Copyright 2026 Chris Piker
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.


"""Discrete Fourier transforms and power spectral density, on numpy.

das2C does these with FFTW, and the C module wraps that as _das3.Dft and
_das3.Psd.  FFTW is under the GPL, so a das3py that must not contain it is
built against a das2C without its transform code (DFT=no in das2C's
makefiles), and the classes here take the place of the C ones.  The Windows
packages on PyPI are built that way.

The constructors, methods and results are the same as the C classes'.  The
arithmetic follows das2C's das3/dft.c, with numpy.fft doing the transform, so
the two agree to rounding error.  test/TestPsd.py compares them wherever
both exist.

Two differences, both on input the C classes handle badly:

  * An input whose length is not the length given to the constructor raises
    ValueError.  The C classes read that many values regardless.

  * Psd raises ValueError for a window name it doesn't know.  The C class
    takes any name and then uses a window of zeros.
"""

import math
import numpy

__all__ = ['Dft', 'Psd']


def _checkLen(uLen):
	uLen = int(uLen)
	if (uLen < 2) or (uLen % 2 != 0):
		raise ValueError(
			"Can't handle odd length DFTs or DFTs less than 2 points long."
		)
	return uLen


def _isHann(sWindow):
	# das2C compares the first four letters, in any case
	return sWindow[:4].upper() == 'HANN'


def _taper(uLen):
	"""1 - cos(2 pi n / (N-1)), the shape under both of das2C's Hann scalings"""
	return 1.0 - numpy.cos( (2.0*math.pi*numpy.arange(uLen)) / (uLen - 1) )


def _inputs(uLen, pReal, pImg):
	"""The input vectors as float64 arrays, checked the way the C module does"""
	aReal = numpy.asarray(pReal, dtype=numpy.float64)
	if aReal.ndim != 1:
		raise ValueError("pReal is not 1-dimensional")

	aImg = None
	if pImg is not None:
		aImg = numpy.asarray(pImg, dtype=numpy.float64)
		if aImg.ndim != 1:
			raise ValueError("pImg is not 1-dimensional")
		if len(aImg) != len(aReal):
			raise ValueError("pReal and pImg must be the same length")

	if len(aReal) != uLen:
		raise ValueError(
			"Input vectors have %d points, this calculator was made for %d"%(
			len(aReal), uLen
		))

	return (aReal, aImg)


class Dft(object):
	"""An amplitude preserving Discrete Fourier Transform converter

	__init__(uLen, sWindow, bForward=True)
		Create a new DFT calculator

		uLen      The length of the data vectors that will be supplied
		          to the calculate function, even and at least 2
		sWindow   A named window to apply to the data.  If None then
		          no window will be used.
		          Accepted values are ['HANN', None]
		bForward  False for the backward transform
	"""

	def __init__(self, uLen, sWindow, bForward=True):
		self._uLen = _checkLen(uLen)

		if sWindow is None:
			self._aWnd = numpy.ones(self._uLen)
		elif _isHann(sWindow):
			# A constant of 2 to make the area under the curve 1.0, as in das2C
			self._aWnd = 2.0 * _taper(self._uLen)
		else:
			raise ValueError("Unknown window function: '%s'"%sWindow)

		self._bForward = bool(bForward)
		self._aOut = None
		self._bRealOnly = False

	def calculate(self, pReal, pImg=None):
		"""Calculate a discrete Fourier transform.

		The result of any earlier calculation is replaced.

		pReal    A "time domain" input vector
		pImg     The imaginary (or quadrature phase) input vector.  For
		         a purely real signal this vector is None

		A ValueError is raised if either is not one dimensional, or not of
		the length given to the constructor.
		"""
		(aReal, aImg) = _inputs(self._uLen, pReal, pImg)

		aIn = (aReal * self._aWnd).astype(numpy.complex128)
		if aImg is not None:
			aIn.imag = aImg * self._aWnd

		if self._bForward:
			self._aOut = numpy.fft.fft(aIn)
		else:
			# FFTW's backward transform is not divided by N, numpy's is
			self._aOut = numpy.fft.ifft(aIn) * self._uLen

		self._bRealOnly = (aImg is None)

	def _result(self):
		if self._aOut is None:
			raise ValueError("calculate() has not been called")
		return self._aOut

	def getReal(self):
		"""Return the real component after a calculation."""
		return self._result().real.copy()

	def getImg(self):
		"""Return the imaginary component after a calculation."""
		return self._result().imag.copy()

	def getMagnitude(self):
		"""Get the amplitude magnitude vector from a calculation.

		Scales the stored DFT so that it preserves amplitude, and gets the
		magnitude.  For real-valued inputs the 'positive' and 'negative'
		frequencies are combined, giving uLen/2 + 1 values.  This is not
		done for complex input, das2C does not either.
		"""
		aOut = self._result()
		if not self._bRealOnly:
			raise NotImplementedError(
				"Magnitude calculation for complex input not yet implemented"
			)

		N = self._uLen
		No2 = N // 2
		aAbs = numpy.abs(aOut)
		aMag = numpy.empty(No2 + 1)
		aMag[0] = aAbs[0]
		aMag[No2] = aAbs[No2]
		aMag[1:No2] = aAbs[1:No2] + aAbs[:No2:-1]  # k with N-k
		return aMag / N

	def getLength(self):
		"""The length of the data vectors that will be supplied to the
		calculate function."""
		return self._uLen


class Psd(object):
	"""Create a new Power Spectral Density calculator.

	This estimator uses the equations given in Numerical Recipes in C,
	section 13.4, but not any of the actual Numerical Recipes source code.

	__init__(uLen, bCenter, sWindow)

		uLen      The length of the data vectors that will be supplied
		          to the calculate function, even and at least 2
		bCenter   If true, input values will be centered on the mean value.
		          This shifts out the DC component from the input
		sWindow   A named window to apply to the data.  If None then
		          no window will be used.
		          Accepted values are ['HANN', None]
	"""

	def __init__(self, uLen, bCenter, sWindow):
		self._uLen = _checkLen(uLen)
		self._bCenter = bool(bCenter)

		# Window, and the window squared and summed (Wss) of Numerical Recipes
		# equation 13.4.11
		if sWindow is None:
			self._aWnd = numpy.ones(self._uLen)
			self._rWss = float(self._uLen) * float(self._uLen)
		elif _isHann(sWindow):
			# The strict Hann function
			self._aWnd = 0.5 * _taper(self._uLen)
			self._rWss = float((self._aWnd * self._aWnd).sum()) * self._uLen
		else:
			raise ValueError("Unknown window function: '%s'"%sWindow)

		self._aMag = numpy.empty(0)
		self._rPwrIn = 0.0
		self._rPwrOut = 0.0

	def calculate(self, pReal, pImg=None):
		"""Calculate a Power Spectral Density (periodogram)

		The result of any earlier calculation is replaced.

		pReal   A "time domain" input vector
		pImg    The imaginary (or quadrature phase) input vector the same
		        length as pReal.  For a purely real signal this vector is
		        None.
		"""
		(aReal, aImg) = _inputs(self._uLen, pReal, pImg)
		N = self._uLen

		# Shift out the DC component, if desired
		if self._bCenter:
			aReal = aReal - (aReal.sum() / N)
			if aImg is not None:
				aImg = aImg - (aImg.sum() / N)

		# Input power, before the window
		rPwrIn = float((aReal * aReal).sum())
		if aImg is not None:
			rPwrIn += float((aImg * aImg).sum())
		self._rPwrIn = rPwrIn / N

		aIn = (aReal * self._aWnd).astype(numpy.complex128)
		if aImg is not None:
			aIn.imag = aImg * self._aWnd

		aOut = numpy.fft.fft(aIn)
		aSq = aOut.real * aOut.real + aOut.imag * aOut.imag

		if aImg is None:
			# Real input: fold the negative frequencies onto the positive
			No2 = N // 2
			aMag = numpy.empty(No2 + 1)
			aMag[0] = aSq[0]
			aMag[No2] = aSq[No2]
			aMag[1:No2] = aSq[1:No2] + aSq[:No2:-1]  # k with N-k
		else:
			aMag = aSq

		# A two point Hann window is all zeros, which makes this 0/0.  The
		# result is NaN as from the C class, without numpy's warning.
		with numpy.errstate(divide='ignore', invalid='ignore'):
			self._aMag = aMag / self._rWss
		self._rPwrOut = float(self._aMag.sum())

	def powerRatio(self, input=False, output=False):
		"""Provide a comparison of the input power and the output power.

		During the calculate() call the average magnitude of the input vector
		is saved along with the average magnitude of the output vector
		(divided by the window summed and squared).  These two measures of
		power should always be close to each other when using a hann window.
		With no window they should be almost identical, to within rounding
		error.  The two measures are:

		                N-1
		            1  ----   2      2
		    Pin =  --- \\    r    +  i
		            N  /     n       n
		               ----
		                n=0

		                  N-1
		             1   ----   2      2
		    Pout =  ---  \\    R    +  I
		            Wss  /     k       k
		                 ----
		                  k=0

		input    (optional) if True include the input power in the return
		output   (optional) if True include the output power in the return

		Returns the ratio of power out divided by power in, or
		(inputPower, powerRatio) with input=True,
		(outputPower, powerRatio) with output=True,
		(inputPower, outputPower, powerRatio) with both.
		"""
		# As C arithmetic gives for a division by zero, without the warning
		if self._rPwrIn != 0.0:
			rRatio = self._rPwrOut / self._rPwrIn
		elif self._rPwrOut == 0.0:
			rRatio = float('nan')
		else:
			rRatio = float('inf')

		if input and output:
			return (self._rPwrIn, self._rPwrOut, rRatio)
		if input:
			return (self._rPwrIn, rRatio)
		if output:
			return (self._rPwrOut, rRatio)
		return rRatio

	def get(self):
		"""Get the power spectral density from a calculation

		For real-valued inputs the 'positive' and 'negative' frequencies are
		combined, giving uLen/2 + 1 values.  For complex input there are uLen
		values, since components above the Nyquist frequency have meaningful
		information.

		Returns a numpy array of float64.  It is empty before the first
		calculate().
		"""
		return self._aMag.copy()
