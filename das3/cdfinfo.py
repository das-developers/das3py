# The MIT License
#
# Copyright 2019-2026 Chris Piker
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

"""The das_cdfinfo command: print the structure of CDF files

Installed as a console script, see [project.scripts] in pyproject.toml and
entry_points in setup.py.  Kept apart from das3.cdf, the CDF writer, so
listing a file needs only pycdf and not the das3 C extension.
"""

# Without this python 2 prints print(a, b) as the tuple (a, b)
from __future__ import print_function

import sys
import argparse
from os.path import basename as bname

try:
	import das3.pycdf as pycdf
	import das3.pycdf.const
except ImportError:
	# If CDF support not installed, try falling back to spacepy
	import spacepy.pycdf as pycdf
	import spacepy.pycdf.const

__all__ = ['main']

# ########################################################################## #

def _compStr(nUnhashable):
	"""Name a CDF compression type.  The CDF c_long types are not hashable,
	so this can't be a dictionary lookup."""
	if nUnhashable == pycdf.const.NO_COMPRESSION : return "No Compression"
	if nUnhashable == pycdf.const.RLE_COMPRESSION : return "RLE Compression"
	if nUnhashable == pycdf.const.HUFF_COMPRESSION : return "Huffman Compression"
	if nUnhashable == pycdf.const.AHUFF_COMPRESSION : return "Adaptive Huffman Compression"
	if nUnhashable == pycdf.const.GZIP_COMPRESSION : return "GZip Compression"

	return str(nUnhashable)

# ########################################################################## #

def main():
	"""Print the global attributes of each CDF file, then each variable with
	its compression, attributes, shape and first record.

	:returns: 0 on success, 13 if a file could not be read
	"""

	psr = argparse.ArgumentParser(
		prog="das_cdfinfo",
		description="A CDF info printer based on pycdf from Triad National Security, LLC"
	)

	psr.add_argument(
		'lFiles', help='The CDF file(s) to describe', nargs='+', metavar='file'
	)

	opts = psr.parse_args()

	for sFile in opts.lFiles:
		try:

			f = pycdf.CDF(sFile)
			print("Structure of: %s"%bname(sFile))

			for ak in f.attrs:
				print(ak, "=", f.attrs[ak])

			print()

			for vk in f.keys():
				print("%s  (%s)"%(vk, _compStr( f[vk].compress()[0] ) ))
				print(f[vk])
				print(f[vk].attrs)

				# Now print the first record
				print( f[vk].shape)
				if len(f[vk].shape) == 1:
					print( f[vk][:] )
				else:
					if f[vk].shape[0] > 0:
						print( f[vk][0,:] )

				print()

			f.close()

		except (ValueError, IOError, pycdf.CDFError) as e:
			sys.stderr.write("%s [ERROR]\n"%str(e))
			return 13

	return 0

# ########################################################################## #
if __name__ == "__main__":
	sys.exit(main())
