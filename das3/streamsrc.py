# -*- coding: utf-8 -*-

# The MIT License
#
# Copyright 2019 Chris Piker
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



"""Data sources reached by HTTP GET, such as das2 and das3 stream servers.

das2C reads the source node and turns requests into URLs, for the 2018 node
layout and the current one alike (see _das3.src_info() and _das3.src_url()).
This module only translates python queries into das2C's keys, tries the
mirrors in order and converts what comes back.  A fix to how nodes are read
reaches das3py with a das2C rebuild.
"""

import sys
import datetime
import _das3
from . source import Source, DAS_MIMES
from . dataset import *
from . dataset import _ds_from_raw, _mk_prop_from_raw
from . util import *

import warnings

try:
	from urllib import quote_plus
except ImportError:
	from urllib.parse import quote_plus

# Names the package re-exports.  Helpers, stdlib imports and module
# globals stay out of 'from das3.streamsrc import *'.
__all__ = [
	'HttpStreamSrc',
]

try:
	basestring
except NameError:
	basestring = str

perr = sys.stderr.write

def _str(val):
	"""A query value as das2C expects it"""
	if isinstance(val, bool): return 'true' if val else 'false'
	if isinstance(val, datetime.datetime): return val.isoformat()
	return '%s'%val

def _table(lRows):
	"""Left aligned columns under a dashed rule, one string per line"""
	lWidth = [max(len(r[i]) for r in lRows) for i in range(len(lRows[0]))]
	def _line(r):
		return '  '.join(r[i].ljust(lWidth[i]) for i in range(len(r))).rstrip()
	lOut = [_line(lRows[0]), _line(['-'*n for n in lWidth])]
	return lOut + [_line(r) for r in lRows[1:]]


# ########################################################################### #
class HttpStreamSrc(Source):
	"""A data source reached by HTTP GET, such as a das stream server.

	Typical use::

		src = das3.get_source('site:/uiowa/tracers/tracers-2/l2/mag_16sps_delta')
		print(src.info())
		(hdr, lDs) = src.get({'time':('2026-05-04T01:16', '2026-05-04T02:52')})
		print(lDs[0])
	"""

	def __init__(self, dDef, bStub, bGlobal):
		"""Constructor is not intended for direct use, use
			das3.get_node()
			das3.get_source()
		instead."""

		if ('type' not in dDef) or (dDef['type'] != 'HttpStreamSrc'):
			raise CatalogError("PyClass, data mismatch, expected 'type' to be "+\
			                   "'HttpStreamSrc' not '%s'"%dDef.get('type'))

		super(HttpStreamSrc, self).__init__(dDef, bStub, bGlobal)
		self._dInfo = None
		self.lBadBase = []   # Mirrors that failed, skipped until all have

	def _loc(self):
		"""What das2C should resolve: the catalog path for a node from the
		global catalog, else the URL it was loaded from."""
		if self.bGlobal: return self.path
		if self.bStub: self.load()
		return self.url

	def _info(self):
		"""das2C's description, see _das3.src_info().  Read once."""
		if self._dInfo is None:
			self._dInfo = _das3.src_info(self._loc())
		return self._dInfo

	# ####################################################################### #
	def examples(self):
		"""List the example datasets this source defines.

		Returns:
			list of (number, label) tuples.  Either one may be given to
			get() to fetch that example.  Some examples produce formats das3py
			doesn't read, such as CDF; get() with no query skips those.
		"""
		return [(i, d['label']) for (i, d) in enumerate(self._info()['examples'])]

	def _exampleIdx(self, where):
		lEx = self._info()['examples']
		if isinstance(where, int):
			if 0 <= where < len(lEx): return where
			raise ValueError("Source %s has no example %d, it has %d"%(
				self.path, where, len(lEx)))
		for (i, d) in enumerate(lEx):
			if d['label'].lower() == where.lower(): return i
		raise ValueError("Source %s has no example labeled '%s', it has: %s"%(
			self.path, where, '; '.join(d['label'] for d in lEx)))

	def _firstUsable(self):
		"""The first example that produces a das stream"""
		lEx = self._info()['examples']
		for (i, d) in enumerate(lEx):
			if d['mime'] in DAS_MIMES: return i
		raise SourceError(self.path, "None of the %d examples produce a das stream"%len(lEx))

	# ####################################################################### #
	@staticmethod
	def _entries(dQuery):
		"""Translate a query dict into das2C (key, value) text entries.

		This goes by the shape of the python values alone and does not look
		at the source: nested dicts join into das2C's dotted keys, so
		{'coord':{'time':{'minimum':x}}} becomes ('coord.time.minimum', x);
		a tuple or list becomes its items joined by commas, which das2C
		reads as a range for a coordinate and as the items of a list
		property; None is skipped, and inside a tuple leaves its slot empty.
		What a key means, and whether its value is acceptable, is das2C's to
		say.  Keep it that way: a rule here that depends on what the node
		defines is a second reader of the node, and goes stale when the
		catalog gains a feature.
		"""
		lOut = []

		def _add(sKey, val):
			if val is None: return
			if isinstance(val, dict):
				for sSub in val: _add("%s.%s"%(sKey, sSub), val[sSub])
			elif isinstance(val, (tuple, list)):
				lOut.append( (sKey, ','.join('' if v is None else _str(v) for v in val)) )
			else:
				lOut.append( (sKey, _str(val)) )

		for sKey in dQuery: _add(sKey, dQuery[sKey])
		return lOut

	def _fetch(self, fUrl, verbose):
		"""Try each mirror in turn.  fUrl(i) gives (url, mime) for base URL i.

		A mirror that fails is skipped by later calls until all of them have
		failed.  The last failure is raised as is, so a server's own error or
		a login refusal isn't hidden behind a generic message.
		"""
		nBases = len(self._info()['base_urls'])
		if nBases == 0:
			raise CatalogError(self.url, "Source %s has no base URLs"%self.path)
		if len(self.lBadBase) >= nBases: self.lBadBase = []

		lTry = [i for i in range(nBases) if i not in self.lBadBase]
		for i in lTry:
			(sUrl, sMime) = fUrl(i)
			if sMime and (sMime not in DAS_MIMES):
				raise SourceError(sUrl, "This request returns %s, das3py reads "
				                  "das streams: %s"%(sMime, ', '.join(DAS_MIMES)))
			if verbose: perr("Requesting: %s\n"%sUrl)
			try:
				(dHdr, lDs) = _das3.read_server(sUrl)
			except Exception as e:
				self.lBadBase.append(i)
				if i == lTry[-1]: raise
				perr("Couldn't read %s, %s; trying the next mirror\n"%(sUrl, str(e)))
				continue

			lOut = [_ds_from_raw(ds) for ds in lDs]
			dRawProps = dHdr['props']
			dHdr['props'] = dict(
				(sProp, _mk_prop_from_raw(dRawProps[sProp])) for sProp in dRawProps
			)
			return (dHdr, lOut)

	# ####################################################################### #
	def get(self, dQuery=None, verbose=False):
		"""Get data from this source.  See Source.get() for the query forms.

		With no query the first example that produces a das stream is used,
		the number or label of an example from examples() selects that one.
		Keys may also be given the way das2C spells them, such as
		'coords.time.min' or 'formats.das.serial', and a coordinate takes a
		single time for the whole span it denotes: {'time':'2026-05-04'} is
		that day.

		Returns:
			(header, datasets), the stream header dict and a list of
			:class:`.Dataset` objects.

		Raises:
			ValueError: if das2C rejects part of the query; the message names
				each bad entry and why.
			SourceError: if the request would return something other than a
				das stream.
		"""
		sLoc = self._loc()
		nEx = -1
		lEnt = []
		if dQuery is None:
			nEx = self._firstUsable()
		elif isinstance(dQuery, (int, basestring)) and not isinstance(dQuery, bool):
			nEx = self._exampleIdx(dQuery)
		elif isinstance(dQuery, dict):
			lEnt = self._entries(dQuery)
		else:
			raise TypeError("Expected a query dict, an example number or label, "
			                "or None, not %s"%type(dQuery).__name__)

		return self._fetch(lambda i: _das3.src_url(sLoc, lEnt, nEx, i), verbose)

	def baseUrls(self):
		"""The servers this source's data can be requested from.

		For the rare case of building a request by hand: add your own GET
		parameters to one of these and give the result to das3.read_http().
		Nothing checks such a URL against the source, and it breaks when the
		server's parameters change, which get() queries are shielded from.
		infoProto() lists the parameters the server takes.

		Returns:
			list of str, in the order get() tries them.
		"""
		return list(self._info()['base_urls'])

	def protoGet(self, dQuery, verbose=False):
		"""Deprecated: send server specific HTTP GET key,value pairs.

		Use get().  To send a hand built request anyway, add parameters to
		one of baseUrls() and call das3.read_http().  There are two ways to
		ask for data, through the source's interface or by URL, and this
		method was a mix of the two.  Removal planned for 3.1.

		Args:
			dQuery (dict) : GET parameter names and values, sent unchecked

		Returns:
			(header, datasets) as for get()
		"""
		warnings.warn(
			"Source.protoGet() is deprecated, use Source.get(), or build a URL "
			"from Source.baseUrls() and call das3.read_http()",
			PendingDeprecationWarning, stacklevel=2
		)
		lBases = self._info()['base_urls']
		lParams = []
		for sKey in dQuery:
			val = dQuery[sKey]
			sVal = quote_plus(val) if isinstance(val, basestring) else _str(val)
			lParams.append("%s=%s"%(sKey, sVal))
		sGet = '&'.join(lParams)

		def _url(i):
			sBase = lBases[i]
			return ("%s%s%s"%(sBase, '&' if '?' in sBase else '?', sGet), '')

		return self._fetch(_url, verbose)

	# ####################################################################### #
	def params(self):
		"""The settable properties of this source, see Source.params().

		Returns:
			{section: {group: {property: dict}}}, where each property dict has
			key (for get()), title, type, units, default, required, param (the
			protocol parameter it travels in), enum, range and multi.  multi
			is None, or for a property that takes a list, the (fewest, most)
			items allowed, most None for no limit; type, enum and range then
			describe one item.  The options section has one group, named ''.
		"""
		dOut = {}
		for d in self._info()['props']:
			if not d['settable']: continue
			dGrp = dOut.setdefault(d['section'], {}).setdefault(d['group'] or '', {})
			dGrp[d['name']] = {
				'key':d['key'], 'title':d['title'], 'type':d['type'] or d['kind'],
				'units':d['units'], 'default':d['value'], 'required':d['required'],
				'param':d['param'], 'enum':d['enum'], 'range':d['range'],
				'multi':d['multi']
			}
		return dOut

	@staticmethod
	def _typeName(d):
		"""A list property is named for its items, as das_fetch does"""
		sType = d['type'] or d['kind']
		return (sType + ' list') if d['multi'] else sType

	@staticmethod
	def _values(d):
		"""What may be given: enum entries, a range, or true and false, then
		how many items a bounded list takes"""
		sOut = ''
		if d['enum']: sOut = ', '.join(d['enum'])
		elif d['range']: sOut = '%s to %s'%d['range']
		elif d['kind'] == 'bool': sOut = 'true, false'

		if d['multi']:
			(nLo, nHi) = d['multi']
			sCount = ''
			if nHi is None:
				if nLo > 1: sCount = '(%d or more)'%nLo
			elif nLo == nHi: sCount = '(%d values)'%nLo
			elif nLo > 1: sCount = '(%d to %d)'%(nLo, nHi)
			else: sCount = '(up to %d)'%nHi
			if sCount: sOut = ('%s %s'%(sOut, sCount)).strip()
		return sOut

	def info(self):
		"""A pretty-print string of this source's examples and settable
		properties, by section.  The Key column is what get() takes."""
		dInfo = self._info()
		lOut = ["%s %s:  %s"%(self.__class__.__name__, dInfo['version'], dInfo['name'])]
		if dInfo['title']: lOut.append("Title:  %s"%dInfo['title'])
		lOut.append("Path:   %s"%dInfo['path'])

		lEx = self.examples()
		if lEx:
			lOut += ['', 'examples:']
			lOut += ['  ' + s for s in _table([('Key', 'Provides')] +
			         [(str(n), s) for (n, s) in lEx])]

		lSecs = []
		for d in dInfo['props']:
			if d['section'] not in lSecs: lSecs.append(d['section'])
		for sSec in lSecs:
			lRows = [d for d in dInfo['props'] if d['settable'] and d['section'] == sSec]
			if not lRows: continue
			lTbl = [('Key', 'Type', 'Units', 'Default', 'Req', 'Values', 'Description')]
			for d in lRows:
				lTbl.append((
					d['key'], self._typeName(d), d['units'],
					'' if d['value'] is None else d['value'],
					'yes' if d['required'] else '', self._values(d), d['title']
				))
			lOut += ['', '%s:'%sSec] + ['  ' + s for s in _table(lTbl)]
		return '\n'.join(lOut)

	def infoProto(self):
		"""A pretty-print string of the HTTP GET parameters behind this
		source's settable properties.  For reading, and for the rare hand
		built URL, see baseUrls()."""
		lTbl = [('Param', 'Req', 'Type', 'Units', 'Set by', 'Description')]
		for d in self._info()['props']:
			if not d['settable']: continue
			lTbl.append((
				d['param'], 'yes' if d['required'] else '', self._typeName(d),
				d['units'], d['key'], d['title']
			))
		lBases = self._info()['base_urls']
		return '\n'.join(['Base URLs:'] + ['  ' + s for s in lBases] + [''] + _table(lTbl))
