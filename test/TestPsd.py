"""Power spectral density and DFT calculators.

das3.PSD is the C class on FFTW when the extension was built with it, and
the numpy class in das3.dft when it was not (the Windows packages, built
without FFTW).  Both have to give the same numbers, so the numpy classes
are checked here against results known in closed form, and against the C
classes wherever those exist.
"""

from __future__ import print_function
import sys
import math
import unittest

import numpy

import das3
import das3.dft
import _das3

# The C classes, if this build has them.  Without FFTW, das3 puts the numpy
# classes on _das3 under the same names.
C_PSD = _das3.Psd if (_das3.Psd is not das3.dft.Psd) else None
C_DFT = _das3.Dft if (_das3.Dft is not das3.dft.Dft) else None


def _signals(N):
	"""(name, real, imaginary or None) test inputs of length N"""
	n = numpy.arange(N, dtype=float)
	rng = numpy.random.RandomState(42)
	return [
		('sine',     3.0*numpy.sin(2*math.pi*5*n/N), None),
		('sine+dc',  1.5 + 0.7*numpy.cos(2*math.pi*3*n/N + 0.3), None),
		('noise',    rng.standard_normal(N), None),
		('iq noise', rng.standard_normal(N), rng.standard_normal(N)),
		('iq tone',  numpy.cos(2*math.pi*7*n/N) + 2.0, numpy.sin(2*math.pi*7*n/N) - 1.0),
		('float32',  rng.standard_normal(N).astype('f4'), None),
	]


class TestPsd(unittest.TestCase):

	def test_public_name(self):
		# das3.PSD is whichever class _das3 carries
		self.assertTrue(das3.PSD is _das3.Psd)

	def test_sine_power(self):
		# A sine of amplitude A has mean square A*A/2, all of it in one bin
		N = 64; A = 3.0; k = 5
		n = numpy.arange(N, dtype=float)
		for cls in (das3.dft.Psd, das3.PSD):
			psd = cls(N, False, None)
			psd.calculate(A*numpy.sin(2*math.pi*k*n/N))
			aPsd = psd.get()
			self.assertEqual(aPsd.shape, (N//2 + 1,))
			self.assertAlmostEqual(aPsd[k], A*A/2.0, places=10)
			aRest = numpy.delete(aPsd, k)
			self.assertTrue(numpy.abs(aRest).max() < 1e-20)

	def test_power_in_equals_power_out(self):
		# Parseval: with no window the two measures are the same
		N = 128
		for (sName, aRe, aIm) in _signals(N):
			for cls in (das3.dft.Psd, das3.PSD):
				psd = cls(N, False, None)
				psd.calculate(aRe, aIm)
				(rIn, rOut, rRatio) = psd.powerRatio(input=True, output=True)
				self.assertAlmostEqual(rRatio, 1.0, places=10, msg=sName)
				self.assertAlmostEqual(rOut/rIn, rRatio, places=12, msg=sName)
				self.assertEqual(len(psd.powerRatio(input=True)), 2)
				self.assertEqual(len(psd.powerRatio(output=True)), 2)
				self.assertTrue(isinstance(psd.powerRatio(), float))

	def test_center_removes_dc(self):
		N = 32
		for cls in (das3.dft.Psd, das3.PSD):
			psd = cls(N, True, 'HANN')
			psd.calculate(numpy.zeros(N) + 7.5)
			self.assertTrue(numpy.abs(psd.get()).max() < 1e-20)

	def test_complex_input_length(self):
		# Complex input keeps every bin, real input folds to N/2 + 1
		N = 16
		a = numpy.arange(N, dtype=float)
		for cls in (das3.dft.Psd, das3.PSD):
			psd = cls(N, False, 'HANN')
			self.assertEqual(psd.get().shape, (0,))   # before any calculate
			psd.calculate(a, a[::-1])
			self.assertEqual(psd.get().shape, (N,))
			psd.calculate(a)
			self.assertEqual(psd.get().shape, (N//2 + 1,))

	def test_bad_arguments(self):
		for cls in (das3.dft.Psd, das3.PSD):
			self.assertRaises(ValueError, cls, 7, False, None)   # odd
			self.assertRaises(ValueError, cls, 0, False, None)
			psd = cls(8, False, None)
			self.assertRaises(ValueError, psd.calculate, numpy.zeros((2, 4)))
			self.assertRaises(ValueError, psd.calculate, numpy.zeros(8), numpy.zeros(6))

		# Checks only the numpy classes make, see das3/dft.py
		self.assertRaises(ValueError, das3.dft.Psd, 8, False, 'BLACKMAN')
		self.assertRaises(ValueError, das3.dft.Psd(8, False, None).calculate, numpy.zeros(6))
		self.assertRaises(ValueError, das3.dft.Dft, 8, 'BLACKMAN')
		self.assertRaises(ValueError, das3.dft.Dft(8, None).getReal)

	def test_dft_amplitude(self):
		# The magnitude of a sine of amplitude A is A, in its own bin
		N = 64; A = 2.5; k = 9
		n = numpy.arange(N, dtype=float)
		dft = das3.dft.Dft(N, None)
		dft.calculate(A*numpy.sin(2*math.pi*k*n/N))
		aMag = dft.getMagnitude()
		self.assertEqual(aMag.shape, (N//2 + 1,))
		self.assertAlmostEqual(aMag[k], A, places=10)
		self.assertEqual(dft.getLength(), N)

	def test_dft_there_and_back(self):
		# The backward transform is not divided by N, as in FFTW
		N = 32
		rng = numpy.random.RandomState(7)
		(aRe, aIm) = (rng.standard_normal(N), rng.standard_normal(N))
		fwd = das3.dft.Dft(N, None, True)
		fwd.calculate(aRe, aIm)
		back = das3.dft.Dft(N, None, False)
		back.calculate(fwd.getReal(), fwd.getImg())
		self.assertTrue(numpy.allclose(back.getReal(), N*aRe, rtol=1e-12, atol=1e-12))
		self.assertTrue(numpy.allclose(back.getImg(),  N*aIm, rtol=1e-12, atol=1e-12))

	def test_numpy_matches_c_psd(self):
		if C_PSD is None:
			print("TestPsd: no C Psd in this build (no FFTW), comparison skipped")
			return
		for N in (4, 16, 512, 1000):
			for sWin in (None, 'HANN'):
				for bCenter in (False, True):
					for (sName, aRe, aIm) in _signals(N):
						c = C_PSD(N, bCenter, sWin)
						p = das3.dft.Psd(N, bCenter, sWin)
						c.calculate(aRe, aIm)
						p.calculate(aRe, aIm)
						sMsg = "%s, N=%d, window=%s, center=%s"%(sName, N, sWin, bCenter)
						# Bins that are zero in exact arithmetic are rounding
						# noise in both, so the tolerance is set by the peak
						rTol = 1e-12 * float(numpy.abs(c.get()).max())
						self.assertTrue(
							numpy.allclose(c.get(), p.get(), rtol=0, atol=rTol), sMsg
						)
						self.assertTrue(numpy.allclose(
							c.powerRatio(input=True, output=True),
							p.powerRatio(input=True, output=True), rtol=1e-12, atol=0
						), sMsg)

	def test_numpy_matches_c_dft(self):
		if C_DFT is None:
			print("TestPsd: no C Dft in this build (no FFTW), comparison skipped")
			return
		for N in (4, 16, 512):
			for sWin in (None, 'HANN'):
				for bForward in (True, False):
					for (sName, aRe, aIm) in _signals(N):
						c = C_DFT(N, sWin, bForward)
						p = das3.dft.Dft(N, sWin, bForward)
						c.calculate(aRe, aIm)
						p.calculate(aRe, aIm)
						sMsg = "%s, N=%d, window=%s, forward=%s"%(sName, N, sWin, bForward)
						rTol = 1e-12 * max(
							float(numpy.abs(c.getReal()).max()),
							float(numpy.abs(c.getImg()).max())
						)
						self.assertTrue(numpy.allclose(
							c.getReal(), p.getReal(), rtol=0, atol=rTol), sMsg
						)
						self.assertTrue(numpy.allclose(
							c.getImg(), p.getImg(), rtol=0, atol=rTol), sMsg
						)
						if aIm is None:
							self.assertTrue(numpy.allclose(
								c.getMagnitude(), p.getMagnitude(), rtol=0, atol=rTol
							), sMsg)


if __name__ == '__main__':
	print("TestPsd: das3.PSD is %s.%s"%(das3.PSD.__module__, das3.PSD.__name__))
	unittest.main()
