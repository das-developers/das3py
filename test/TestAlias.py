"""The das2 name is an alias for the das3 package, on both interpreters.

Every das2.* name must be the same object as its das3.* counterpart: the
package, a submodule imported by dotted name, a subpackage module, a class
and a C extension function.  A second copy of any of these would make
isinstance() checks fail across the boundary, which is the failure the
shim's meta path finder exists to prevent.
"""

from __future__ import print_function
import sys
import warnings

perr = sys.stderr.write


def _testOldNames(das2):
	"""The 2.3 method names that das2py 2.3's examples used still answer,
	with one PendingDeprecationWarning each, the same as the 3.0 names."""
	nBad = 0

	class _Src(das2.Source):
		# A stand-in for a catalog source, no network
		def getProto(self, dConfig, verbose=False): return ('get', dConfig, verbose)
		def infoProto(self): return 'info'
	src = _Src.__new__(_Src)   # skip Node construction, it wants a catalog dict

	ds = das2.Dataset('test')
	dim = das2.Dimension(ds, 'frequency', 'coord')
	dim.props['scaleType'] = 'log'

	t = das2.DasTime('2026-09-13T01:02:03.456')

	lCases = (
		('DasTime.from_string', lambda: das2.DasTime.from_string('2026-09-13'),
		                        das2.DasTime.fromString('2026-09-13')),
		('DasTime.round_doy',   lambda: t.round_doy(t.MILLISEC), t.roundDoy(t.MILLISEC)),
		('Dimension.propEq',    lambda: dim.propEq('scaleType', 'log'), True),
		('Source.protoGet',     lambda: src.protoGet({'a':1}, verbose=True),
		                        ('get', {'a':1}, True)),
		('Source.protoInfo',    lambda: src.protoInfo(), 'info'),
	)
	for (sName, fOld, expect) in lCases:
		with warnings.catch_warnings(record=True) as lWarn:
			warnings.simplefilter('always')
			got = fOld()
		nDep = len([w for w in lWarn if issubclass(w.category, PendingDeprecationWarning)])
		if nDep != 1:
			nBad += 1
			perr("FAIL: %s() gave %d PendingDeprecationWarnings, expected 1\n"%(sName, nDep))
		if got != expect:
			nBad += 1
			perr("FAIL: %s() returned %r, expected %r\n"%(sName, got, expect))

	return nBad


def main(argv):
	nBad = 0

	with warnings.catch_warnings(record=True) as lWarn:
		warnings.simplefilter('always')
		import das2
		import das3
		import das2.dastime
		import das2.pycdf.const
		from das2.dataset import Dataset as DsViaAlias

	lDep = [w for w in lWarn if issubclass(w.category, PendingDeprecationWarning)]
	if len(lDep) != 1:
		nBad += 1
		perr("FAIL: expected one PendingDeprecationWarning from 'import das2', got %d\n"%len(lDep))

	lPairs = (
		('package',          das2,                 das3),
		('submodule',        das2.dastime,         das3.dastime),
		('subpackage',       das2.pycdf.const,     das3.pycdf.const),
		('class',            das2.DasTime,         das3.DasTime),
		('class via from',   DsViaAlias,           das3.Dataset),
		('extension func',   das2._das3.read_file, das3._das3.read_file),
	)
	for (sWhat, a, b) in lPairs:
		if a is not b:
			nBad += 1
			perr("FAIL: das2 %s is not the das3 object\n"%sWhat)

	for sName in ('das2', 'das2.dastime', 'das2.pycdf.const'):
		if sys.modules.get(sName) is not sys.modules[sName.replace('das2', 'das3', 1)]:
			nBad += 1
			perr("FAIL: sys.modules[%r] is a separate module\n"%sName)

	# The thing the alias is for: an old script's objects mix with new ones
	t = das2.DasTime('2026-09-13')
	if not isinstance(t, das3.DasTime):
		nBad += 1
		perr("FAIL: a das2.DasTime is not a das3.DasTime\n")

	nBad += _testOldNames(das2)

	if nBad:
		return 13
	print("das2 aliases das3 (%s)"%sys.version.split()[0])
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv))
