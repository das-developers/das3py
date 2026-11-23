"""Data sources through das2C's DasSrc and DasReq, offline.

The node fixtures are copies of real catalog nodes, all HttpStreamSrc 0.7:
the Tracers MAG delta-B source set and its source on a dasflex server, and
a Cassini MAG source on a das2 server, whose protocol is quite different.
The 2018 node layout is not covered; das2C stopped reading it after
v3.0.6.  node_multi_0.7.json
is das2C's own synthetic fixture for list properties,
test/das_fetch/multi_0.7.json there; refresh it from das2C, don't edit it
here.  Nodes are loaded
from these files and URLs are built but never fetched; a stand-in for
_das3.read_server() records what would have been requested.

URLs are compared by their query parameters, not as strings, so a change in
the order das2C writes them doesn't fail the test.
"""

from __future__ import print_function
import os
import sys
import warnings

try:
	from urlparse import urlparse, parse_qs
except ImportError:
	from urllib.parse import urlparse, parse_qs

import das3
import das3.streamsrc

perr = sys.stderr.write

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
FLEX = os.path.join(TEST_DIR, 'node_tracers_mag_flex.json')
SSET = os.path.join(TEST_DIR, 'node_tracers_mag_set.json')
CASS = os.path.join(TEST_DIR, 'node_cassini_fce_das2.json')
MULT = os.path.join(TEST_DIR, 'node_multi_0.7.json')

TRACERS_BASE = 'https://space.physics.uiowa.edu/das/stream/source/tracers/tracers-2/l2/mag_16sps_delta/flex'
CASSINI_BASE = 'https://planet.physics.uiowa.edu/das/das2Server'

GEI = 'ts2_l2_mag_16sps_gei2000_deltab'
FAC = 'ts2_l2_mag_16sps_fac_deltab'


class Failures(object):
	def __init__(self): self.n = 0
	def check(self, bOkay, sMsg):
		if not bOkay:
			self.n += 1
			perr("FAIL: %s\n"%sMsg)


def _split(sUrl):
	"""(base, {param: value}) with each value unquoted"""
	u = urlparse(sUrl)
	dQ = dict((k, v[0]) for (k, v) in parse_qs(u.query, keep_blank_values=True).items())
	return ("%s://%s%s"%(u.scheme, u.netloc, u.path), dQ)


class _Recorder(object):
	"""Stands in for _das3.read_server(), keeps the URLs it's asked for"""
	def __init__(self): self.lUrls = []
	def __call__(self, sUrl, *args):
		self.lUrls.append(sUrl)
		return ({'props':{}}, [])


def checkInfo(fails):
	d = das3._das3.src_info(FLEX)
	fails.check(d['version'] == '0.7', "flex version %r"%d['version'])
	fails.check(d['base_urls'] == [TRACERS_BASE], "flex base_urls %r"%d['base_urls'])
	lEx = [(e['label'], e['mime']) for e in d['examples']]
	fails.check(len(lEx) == 2 and lEx[1][1] == 'application/x-cdf',
		"flex examples %r, expected the second to be CDF"%lEx)

	dProps = dict((p['key'], p) for p in d['props'])
	p = dProps.get('coords.time.min')
	fails.check(p is not None and p['required'] and p['param'] == 'read.time.min'
		and p['kind'] == 'time', "flex coords.time.min %r"%p)
	p = dProps.get('data.%s.enabled'%FAC)
	fails.check(p is not None and p['value'] == 'true' and p['kind'] == 'bool',
		"flex FAC enable %r"%p)

	d = das3._das3.src_info(CASS)
	fails.check(d['version'] == '0.7', "cassini version %r"%d['version'])
	dProps = dict((p['key'], p) for p in d['props'])
	p = dProps.get('coords.time.res')
	fails.check(p is not None and p['param'] == 'resolution' and p['units'] == 's',
		"cassini coords.time.res %r"%p)


# (fixture, query, expected params).  Every row is one of the query forms in
# Source.get()'s docstring or the das2C spellings HttpStreamSrc.get() adds.
QUERIES = (
	(FLEX, {'time':('2026-05-04T01:29:59', '2026-05-04T01:38:07')},
		{'read.time.min':'2026-05-04T01:29:59', 'read.time.max':'2026-05-04T01:38:07'}),
	(FLEX, {'time':'2026-05-04'},
		{'read.time.min':'2026-05-04T00:00:00', 'read.time.max':'2026-05-05T00:00:00'}),
	(FLEX, {'time':('2026-05-04', '2026-05-05'), GEI:True, FAC:False},
		{'read.time.min':'2026-05-04', 'read.time.max':'2026-05-05', 'read.data':GEI}),
	(FLEX, {'coords.time.min':'2026-05-04', 'coords.time.max':'2026-05-05',
	        'formats.das.serial':'text'},
		{'read.time.min':'2026-05-04', 'read.time.max':'2026-05-05', 'format.serial':'text'}),
	(FLEX, {'time':('2026-05-04', '2026-05-05'), 'das':{'serial':'text'}},
		{'read.time.min':'2026-05-04', 'read.time.max':'2026-05-05', 'format.serial':'text'}),
	(CASS, {'coord':{'time':{'minimum':'2017-01-02', 'maximum':'2017-01-03'}}},
		{'server':'dataset', 'dataset':'Cassini/MAG/ElectronCyclotron',
		 'start_time':'2017-01-02', 'end_time':'2017-01-03'}),
	(CASS, {'time':('2017-01-02', '2017-01-03', 60)},
		{'server':'dataset', 'dataset':'Cassini/MAG/ElectronCyclotron',
		 'start_time':'2017-01-02', 'end_time':'2017-01-03', 'resolution':'60'}),
	(CASS, {'time':('2017-01-02', None, None)},
		None),   # likewise on the das2 server node
	(FLEX, {'time':('2026-05-04', None)},
		None),   # no end time and no default: must be refused
)


def checkQueries(fails):
	rec = _Recorder()
	readSave = das3.streamsrc._das3.read_server
	das3.streamsrc._das3.read_server = rec
	try:
		dSrc = {
			FLEX: das3.get_node('test:/tracers/mag/flex', FLEX),
			CASS: das3.get_node('test:/cassini/fce/das2', CASS),
		}
		for (sFile, dQuery, dExpect) in QUERIES:
			src = dSrc[sFile]
			sWho = "%s %r"%(os.path.basename(sFile), dQuery)
			try:
				src.get(dQuery)
			except ValueError as e:
				fails.check(dExpect is None, "%s refused: %s"%(sWho, e))
				continue
			if dExpect is None:
				fails.check(False, "%s accepted, expected a refusal"%sWho)
				continue
			(sBase, dGot) = _split(rec.lUrls[-1])
			fails.check(dGot == dExpect, "%s gave %r, expected %r"%(sWho, dGot, dExpect))

		# Example selection: none given means the first one producing a das
		# stream; asking for the CDF example is refused before any request
		nBefore = len(rec.lUrls)
		dSrc[FLEX].get()
		(sBase, dGot) = _split(rec.lUrls[-1])
		fails.check(sBase == TRACERS_BASE and len(rec.lUrls) == nBefore + 1,
			"flex default example went to %s"%rec.lUrls[-1])
		try:
			dSrc[FLEX].get(1)
			fails.check(False, "flex CDF example was fetched")
		except das3.SourceError:
			pass
		dSrc[CASS].get('Example 00')   # by label
		(sBase, dGot) = _split(rec.lUrls[-1])
		fails.check(dGot.get('start_time') == '2016-12-11T13:00',
			"cassini example by label gave %r"%dGot)

		# The 2.3 protoGet() still sends exactly what it's given, and says
		# once that it's on the way out
		with warnings.catch_warnings(record=True) as lWarn:
			warnings.simplefilter('always')
			dSrc[FLEX].protoGet({'read.time.min':'2026-05-04', 'read.time.max':'2026-05-05'})
		nDep = len([w for w in lWarn if issubclass(w.category, PendingDeprecationWarning)])
		fails.check(nDep == 1, "protoGet gave %d PendingDeprecationWarnings, expected 1"%nDep)
		(sBase, dGot) = _split(rec.lUrls[-1])
		fails.check(sBase == TRACERS_BASE and dGot ==
			{'read.time.min':'2026-05-04', 'read.time.max':'2026-05-05'},
			"flex protoGet gave %s"%rec.lUrls[-1])
		fails.check(not hasattr(dSrc[FLEX], 'getProto'), "getProto() is back")
		fails.check(dSrc[FLEX].baseUrls() == [TRACERS_BASE],
			"flex baseUrls %r"%dSrc[FLEX].baseUrls())

		# Bad keys and values come back from das2C with its reason
		for dBad in ({'nosuch':1, 'time':'2026-05-04'},
		             {'time':'2026-05-04', FAC:'maybe'}):
			try:
				dSrc[FLEX].get(dBad)
				fails.check(False, "flex accepted %r"%dBad)
			except ValueError as e:
				fails.check('Request refused' in str(e), "flex %r: %s"%(dBad, e))
	finally:
		das3.streamsrc._das3.read_server = readSave


def checkSourceSet(fails):
	node = das3.get_node('test:/tracers/mag', SSET)
	fails.check(isinstance(node, das3.SourceSet), "set node is a %s"%type(node).__name__)

	# The 2.3 name is the same class, so old isinstance() tests still hold
	fails.check(das3.Collection is das3.SourceSet, "das3.Collection is not SourceSet")
	fails.check(isinstance(node, das3.Collection), "set node is not a das3.Collection")

	# Children das3py has no class for are still listed
	fails.check(sorted(node.keys()) == ['das2', 'flex'], "set children %r"%sorted(node.keys()))

	src = node.source()
	fails.check(isinstance(src, das3.HttpStreamSrc) and src.path.endswith('/flex'),
		"set picked %s %s"%(type(src).__name__, src.path))


def checkLists(fails):
	"""List properties: python sends the items, das2C does the rest"""
	d = das3._das3.src_info(MULT)
	dProps = dict((p['key'], p) for p in d['props'])
	for (sKey, expect) in (('options.lfdr', (0, None)), ('options.notches', (1, 3)),
	                       ('options.pair', (2, 2)), ('options.bg_div', None)):
		p = dProps.get(sKey)
		fails.check(p is not None and p['multi'] == expect,
			"multi of %s is %r, expected %r"%(sKey, p and p['multi'], expect))

	src = das3.get_node('test:/multi', MULT)
	sInfo = src.info()
	fails.check('real list' in sInfo and '(up to 3)' in sInfo,
		"info() doesn't show notches as a bounded real list")
	fails.check(src.params()['options']['']['pair']['multi'] == (2, 2),
		"params() multi for pair")

	rec = _Recorder()
	readSave = das3.streamsrc._das3.read_server
	das3.streamsrc._das3.read_server = rec
	try:
		# A list or a tuple, in any order and case; das2C puts enum items in
		# node order and joins them the way the server wants
		src.get({'lfdr':['ew', 'EX'], 'nix':('fake_data', 'mfdr_mfr2')})
		(sBase, dGot) = _split(rec.lUrls[-1])
		fails.check(dGot.get('params') == '-lfdr ExEw -n mfdr_mfr2 fake_data',
			"list query gave params %r"%dGot.get('params'))

		src.get({'option':{'notches':[2.4, 2.5]}, 'dft':'fft512'})
		(sBase, dGot) = _split(rec.lUrls[-1])
		fails.check(dGot.get('params') == 'fft512 2.4 2.5',
			"numeric list gave params %r"%dGot.get('params'))

		# Too many items, an item outside the enum, and the 2.3 habit of a
		# list of things to switch on: all das2C's to refuse
		for dBad in ({'notches':[1, 2, 3, 4]}, {'lfdr':['Ex', 'Ez']},
		             {'bac_fac':['enabled']}):
			nBefore = len(rec.lUrls)
			try:
				src.get(dBad)
				fails.check(False, "multi accepted %r as %s"%(dBad, rec.lUrls[-1]))
			except ValueError as e:
				fails.check(len(rec.lUrls) == nBefore, "multi %r was sent"%dBad)
	finally:
		das3.streamsrc._das3.read_server = readSave


def checkSession(fails):
	"""The seed and the mounts, with files so nothing is fetched.  The
	session is the module's own, so put it back the way it was."""
	sSet = das3.DEF_CATALOG_TAG + 'test:/tracers/mag'
	lMounts = das3.mounts()
	tSeed = das3.seed_info()
	fails.check(tSeed == (None, ''), "the seed starts as %r"%(tSeed,))
	try:
		das3.seed(SSET, 'test:/tracers/mag')
		fails.check(das3.seed_info() == (SSET, sSet), "seed_info %r"%(das3.seed_info(),))
		das3.mount(FLEX, 'test:/tracers/mag/flex')
		fails.check(das3.mounts() == lMounts + [(FLEX, sSet + '/flex')],
			"mounts %r"%das3.mounts())
		das3.check()

		# A path relative to the seed, answered by the mount.  The node's
		# name is the full one, not the text asked for.
		src = das3.get_source('flex')
		fails.check(src.path == sSet + '/flex', "relative path gave %s"%src.path)

		# A path ending at the set gives its source, fully loaded
		src = das3.get_source('test:/tracers/mag')
		fails.check(src.path == sSet + '/flex' and (not src.bStub) and ('interface' in src.props),
			"get_source() on a set gave %s, stub %s"%(src.path, src.bStub))
		fails.check(das3.node_dict('flex')['type'] == 'HttpStreamSrc', "node_dict('flex')")
		fails.check(das3.get_node('test:/tracers/mag').path == sSet, "full name under the seed")

		das3.unmount('test:/tracers/mag/flex')
		fails.check(das3.mounts() == lMounts, "mounts after unmount %r"%das3.mounts())
		try:
			das3.unmount('test:/tracers/mag/flex')
			fails.check(False, "unmounting twice raised nothing")
		except das3._das3.Error:
			pass

		das3.mount(os.path.join(TEST_DIR, 'no_such_node.json'), 'test:/nowhere')
		try:
			das3.check()
			fails.check(False, "check() passed a mount of a missing file")
		except das3._das3.Error as e:
			fails.check('no_such_node.json' in str(e), "check() said: %s"%e)
		das3.unmount('test:/nowhere')
	finally:
		das3.seed()
		for (sUrl, sUri) in das3.mounts():
			if (sUrl, sUri) not in lMounts: das3.unmount(sUri)
	fails.check(das3.seed_info() == (None, ''), "seed not restored")


def main(argv):
	fails = Failures()
	for f in (checkInfo, checkQueries, checkSourceSet, checkLists, checkSession):
		try:
			f(fails)
		except Exception as e:
			fails.check(False, "%s: %s: %s"%(f.__name__, type(e).__name__, e))
	if fails.n > 0:
		perr("%d data source checks failed\n"%fails.n)
		return 13
	print("Data sources read and queried through das2C (%s)"%sys.version.split()[0])
	return 0

if __name__ == '__main__':
	sys.exit(main(sys.argv))
