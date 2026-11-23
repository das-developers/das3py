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
"""Looking up data sources and sub catalogs of das catalogs"""

import _das3
from . node import *
from . node import Node
from . util import CatalogError
from . source import *
from . source import DAS_MIMES
from . streamsrc import *

# Names the package re-exports.  Helpers, stdlib imports and module
# globals stay out of 'from das3.container import *'.
__all__ = [
	'Catalog',
	'Collection',
	'SourceSet',
]


try:
  basestring
except NameError:
  basestring = str

# ########################################################################### #
class Catalog(Node):
	"""
	Catalog objects.  May contain other Catalogs, SourceSets or SpaseRec
	objects.
	"""
	def __init__(self, dDef, sSubKey, bStub, bGlobal):
		super(Catalog, self).__init__(dDef, bStub, bGlobal)
		self.sSubKey = sSubKey

		# If I'm a fully realized node then I'll need to initialize minimal child
		# objects
		self.subs = None
		if not self.bStub:
			self._load_stubs()

	def _load_stubs(self):
		# Load stub versions of sub items

		self.subs = {}
		for sKey in self.props[self.sSubKey]:

			dDef = self.props[self.sSubKey][sKey]

			# Assign a name based on how this appeared in the subitems
			sSep = '/'
			if 'separator' in self.props:
				if self.props['separator'] == None: sSep = ''
				else: sSep = self.props['separator']

			dDef['_path'] = "%s%s%s"%(self.props['_path'], sSep, sKey)

			if 'type' not in dDef:
				raise CatalogError("'type' missing in sources sub-item %s from %s"%(
						              	sKey, self.url))

			self.subs[sKey] = _stub(dDef, self.bGlobal)

	def load(self):
		super(Catalog, self).load()
		self._load_stubs()


	# Providing the dictionary interface, take from page:
	# https://docs.python.org/3/reference/datamodel.html?emulating-container-types#emulating-container-types

	def __len__(self):
		if self.bStub: self.load()
		return len(self.subs)

	def __getitem__(self, key):
		if not isinstance(key, basestring):
			raise TypeError("Expected a string type for sub item key")
		if self.bStub: self.load()
		return self.subs[key]


	# Not settable, skipping __setitem__, __delitem__
	def __iter__(self):
		if self.bStub: self.load()
		return self.subs.__iter__()

	def __contains__(self,key):
		if not isinstance(key, basestring):
			raise TypeError("Expected a string type for sub item key")
		if self.bStub:	self.load()
		return self.subs.__contains__(key)

	def keys(self):
		if self.bStub: self.load()
		return self.subs.keys()

# ########################################################################### #
def _stub(dDef, bGlobal):
	"""A not yet loaded child of a container.  Node types das3py has no
	class for become plain Nodes, so a container can always be listed even
	when some children can't be used from python."""
	sType = dDef['type']
	if sType == 'Catalog':       return Catalog(dDef, 'catalog', STUB, bGlobal)
	if sType == 'SourceSet':     return SourceSet(dDef, STUB, bGlobal)
	if sType == 'HttpStreamSrc': return HttpStreamSrc(dDef, STUB, bGlobal)
	return Node(dDef, STUB, bGlobal)

# ########################################################################### #
class SourceSet(Catalog):
	"""Several sources for one dataset.

	Each child is a different way to get the same data: a das stream server,
	a CDF archive, and so on.  source() picks the one das3py reads best, so a
	path ending at a SourceSet works with get_source() like a path ending at
	the source itself.
	"""

	def __init__(self, dDef, bStub, bGlobal):

		if ('type' not in dDef) or (dDef['type'] != 'SourceSet'):
			raise CatalogError("PyClass, data mismatch, expected 'type' to be "+\
			                   "'SourceSet' not '%s'"%dDef.get('type'))

		super(SourceSet, self).__init__(dDef, 'catalog', bStub, bGlobal)

	def source(self):
		"""The child source das3py reads by default.

		das2C lists which children it can load and each child says which MIME
		types it provides.  Which to use is das3py's choice, not das2C's: the
		first of das3 binary, das3 text, das2 binary and das2 text that any
		loadable child provides, preferring a child whose purpose is
		'primary-stream' and then node order.  A loadable child that doesn't
		say what it provides is taken last.

		Returns: A Source, not yet loaded.

		Raises: CatalogError if no loadable child provides a das stream.
		"""
		if self.bStub: self.load()

		sLoc = self.path if self.bGlobal else self.url
		lNames = [s for s in _das3.src_loadable(sLoc) if s in self.subs]
		dDefs = self.props[self.sSubKey]

		def _pick(lHit):
			lPrime = [s for s in lHit if dDefs[s].get('purpose') == 'primary-stream']
			return self.subs[(lPrime or lHit)[0]]

		for sMime in DAS_MIMES:
			lHit = [s for s in lNames if sMime in (dDefs[s].get('provides') or [])]
			if lHit: return _pick(lHit)

		lHit = [s for s in lNames if not dDefs[s].get('provides')]
		if lHit: return _pick(lHit)

		lHave = ["%s (%s)"%(s, ', '.join(dDefs[s].get('provides') or [])) for s in self.subs]
		raise CatalogError(self.url,
			"No source in %s provides a das stream, it has: %s"%(self.path, '; '.join(lHave))
		)


# The das2py 2.3 name.  In the 2018 catalog layout a dataset's set of sources
# was a 'Collection' node; SourceSet nodes took their place and nothing serves
# the old ones.  An alias and not a subclass, so that isinstance(node,
# Collection) in an old script is true for the nodes it gets today.  It can't
# warn the way the renamed methods do, a name is not a call.  Keep it for a
# while: 'import das2' scripts reach it as das2.Collection.
Collection = SourceSet
