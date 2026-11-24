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


FILL = -1.0e+31

# Das catalog URIs use the RFC 4151 tag scheme.  Any tag authority works
# with the catalog system; this is the one the library fills in when a
# source path is given without one.
DEF_CATALOG_TAG = 'tag:das2.org,2012:'

__version__ = '3.0.7'

# Only the type comments below name Tuple, and those are read statically,
# so the name need not stay on the package.
try:
	from typing import Tuple
	del Tuple
except ImportError:
	pass
	
import _das3

# The power spectral density calculator.  _das3 has one, on FFTW, unless it
# was built against a das2C without its transform code, as the Windows
# packages are, to keep FFTW (GPL) out of them.  Then the numpy ones in
# das3.dft are used, which give the same results.  They are also put on
# _das3 under the names the C module uses, so that code written against the
# low-level module finds them in either kind of build.
from das3 import dft
if not hasattr(_das3, 'Psd'):
	_das3.Psd = dft.Psd
	_das3.Dft = dft.Dft
from _das3 import Psd as PSD

from das3.dastime   import *
from das3.source    import *
from das3.streamsrc import * 
from das3.container import *
from das3.node      import GLOBAL, DETACHED, STUB, FULL
from das3.dataset   import *
from das3.auth      import *
from das3.util      import *
from das3.reader    import *

# Pull up a function or two from the C module:
from _das3 import convert
from _das3 import convertible
from _das3 import parse_epoch as from_epoch;
from _das3 import to_epoch;

import das3.toml as toml
import das3.pkt as pkt
import das3.cli as cli

# ########################################################################### #
# Where catalog branches come from

def _fullName(sPathId):
	"""Expand das3py's 'site:' and 'test:' shorthands to full catalog names.

	das2C takes names as written and knows no shorthands, so every name
	headed for _das3 passes through here first.  Anything else, None
	included, comes back unchanged.
	"""
	if sPathId and (sPathId.startswith('site:') or sPathId.startswith('test:')):
		return DEF_CATALOG_TAG + sPathId
	return sPathId

def mount(sUrl, sUri):
	"""Load a catalog branch from a given location instead of the das catalog.

	Paths under sUri load from sUrl and shadow what the das catalog says for
	them; everything else resolves as usual.  Handy for a development server
	or a local copy of a branch.  Nothing is fetched until a path under sUri
	is used, so a location that doesn't answer is found out then, not here.

	May be called at any time.  Mounting at a name already mounted replaces
	the earlier mount.

	Args:
		sUrl (str) : Where to load the branch's top node from, a URL or file
		sUri (str) : The catalog name to put it at, for example 'site:/uiowa'.
			A name does not end with the separator its children are joined
			by: 'site:/uiowa', not 'site:/uiowa/'.

	Raises:
		_das3.Error : if sUrl isn't a location or sUri isn't a name
	"""
	_das3.mount(sUrl, _fullName(sUri))

def unmount(sUrlOrUri):
	"""Remove a catalog mount, by name or by location.

	Args:
		sUrlOrUri (str) : A name given to mount(), matched exactly, so
			'site:/uiowa' removes a mount at site:/uiowa and none below it.
			Or a location, which removes every mount loaded from it.

	Raises:
		_das3.Error : if nothing matched
	"""
	_das3.unmount(_fullName(sUrlOrUri))

def mounts():
	"""List the catalog mounts.

	Returns:
		list: (url, uri) tuples in the order mounted.  The uri is the full
		catalog name, shorthands expanded.  The seed is not a mount, see
		seed_info().
	"""
	return _das3.mounts()

def seed(sUrl=None, sUri=None):
	"""Choose the node that catalog paths start from.

	In a new process paths start at the root of the das catalog, so every
	path is a full name ('site:/...' being short for
	'tag:das2.org,2012:site:/...').  A seed changes that:

	  * seed(None, 'site:/uiowa') starts at that node of the das catalog, so
	    'juno/wav/survey' is site:/uiowa/juno/wav/survey.  Full names under
	    the seed still work.  The rest of the catalog can't be reached,
	    except through mounts.

	  * seed('./uiowa.json', 'site:/uiowa') does the same from a file, which
	    with mount() is how an offline catalog is put together.  Children
	    load from the URLs the file lists, so one local file doesn't make
	    the tree below it local.

	  * seed('./tree.json') makes the file a root: it has no name and paths
	    are names from it.

	  * seed() goes back to the das catalog root.

	Mounts are kept.  Nothing is fetched until a lookup needs the seed; use
	check() to find a bad location now.

	Args:
		sUrl (str, optional) : A URL or file of a catalog node
		sUri (str, optional) : The catalog name the node has

	Raises:
		_das3.Error : if sUrl isn't a location or sUri is one
	"""
	_das3.seed(sUrl, _fullName(sUri))

def seed_info():
	"""Where catalog paths start now.

	Returns:
		tuple: (url, uri).  url is None when the seed comes from the das
		catalog root built into das2C, which is a list of mirrors rather
		than one location.  uri is '' when the seed is a root.
	"""
	return _das3.seed_info()

def check():
	"""Load the seed and the top node of every mount now.

	These are otherwise loaded by the first lookup that needs them, which is
	a poor time to learn that a location was mistyped.  Nodes loaded here
	are kept, so later lookups cost nothing extra.

	Raises:
		_das3.Error : naming the first that would not load
	"""
	_das3.check()

# ########################################################################### #
# The basic data reading functions

def read_cmd(sCmd):
	# type: (str) -> Tuple[dict, list]
	"""Run a das2 reader command line and output a list of das2 datasets

	Args:
		sCmd (str) : The command line to run in a subshell, may contain
			pipes and other borne shell (posix) or cmd shell (windows)
			constructs.  It is the callers responsibility to check for dangerous
			shell escapes.

	Returns: (dict, list)
		A stream header followed by a list of dataset objects created from the
		message body.  The return datasets may or may not have data depending
		on if data packetes were part of the response.

	Raises:
		_das3.Error : If the command cannot be run, or the stream does not parse.
	"""

	(dHdr, lDs) = _das3.read_cmd(sCmd)
	return (dHdr, [dataset._ds_from_raw(ds) for ds in lDs])



def read_file(sFileName):
	# type: (str)  -> Tuple[dict, list]
	"""Read datasets from a file

	Args:
		sFileName (str) : the name of the file to read

	Returns: (dict, list)
		A stream header followed by a list of dataset objects created from the
		message body.  The return datasets may or may not have data depending
		on if data packetes were part of the response.

	Raises:
		_das3.Error : If the file cannot be read, or the stream does not parse.
	"""

	(dHdr, lDs) = _das3.read_file(sFileName)
	return (dHdr, [dataset._ds_from_raw(ds) for ds in lDs])


def read_http(sUrl, rTimeOut=3.0, sAgent=None):
	#type: (str, float, str) -> Tuple[dict, list]
	"""Issue an HTTP GET command to a remote server and output a list of
	datasets.

	To prevent this function from prompting for authentication on the standard
	input terminal, you can use the das3.set_auth() or das3.load_auth()
	functions before calling das3.read_http().

	Args:
		sUrl (str) : The URL, with all GET parameters if necssary to use to
			contact the server.  Must start with either 'http' or 'https'.

		rTimeOut (float) : The maximum amount of time to wait in seconds for the
			TCP/IP connection to be established.  A value of 0.0 will wait until
			the operating system itself gives up, typically over five minutes.
			This value only specifies a maximum wait for the initial connection,
			actual data download times are not limited by this library.

		sAgent (str, options) : The user-agent string to set in the HTTP/HTTPs
			header.  If not specified a default string will be sent.

	Returns: (dict, list)
		A stream header followed by a list of dataset objects created from the
		message body.  The return datasets may or may not have data depending
		on if data packetes were part of the response.

	Raises:
		_das3.Error : If the server cannot be reached or returns an error, or
			the stream does not parse.
	"""

	if sAgent:
		(dHdr, lDs) = _das3.read_server(sUrl, rTimeOut, sAgent)
	else:
		(dHdr, lDs) = _das3.read_server(sUrl, rTimeOut)

	return (dHdr, [dataset._ds_from_raw(ds) for ds in lDs])


# ########################################################################### #
#
# Catalog Classes:
#                     Node
#                      |
#          +-----------+-----------------+
#          |                             |
#        Source                       Catalog
#          |                             |
#      +-----------+             +--------+----------+
#      |           |             |        |          |
# HttpStreamSrc  FileAggSrc  SpaseCat  DasCat   SourceSet
#

def get_node(sPathId, sUrl=None):
	"""Read a single das2 catalog node from a local or remote file.

	Args:
		sPathId (str) : A string providing the catalog Path ID for the object to
			load.  Since the das2 root node at 'tag:das2.org,2012: contains the
			most commonly loaded item, any sPathId starting with 'site:/' or
			'test:/' is assumed to sub item of the das2 root node.

		.. note: This value is required even if an explicit URL is provided
			because much like files on a disk, das2 catalog objects do not embbed
			the name of node within the contents of the node.

		sUrl (str, optional) : A string providing a direct load URL.
			URLs must start with one of 'file://', 'http://', or 'https://'.

	Returns:
		Either a das3.Catalog, das3.SourceSet or das3.HttpStreamSrc
		object depending on the contents of the file.  If the file does not
		describe one of these object types or can't be read then None is
		returned.

	Examples:

		Load an http stream source for a das2 Voyager dataset:

			>>> src = get_node('site:/uiowa/voyager/1/pws/uncalibrated/waveform/das2')
			>>> print(src.__class__.__name__)
			HttpStreamSrc

		Load the catalog of das2 production sites:

			>>> cat = get_node('site:/uiowa')
			>>> print(cat.__class__.__name__)
			Catalog

		Load the same item as above but provide the full catalog path URI:

			>>> cat = get_node('tag:das2.org,2012:site:/uiowa')

		Load a SPASE record for the JAXA  listing

			>>> rec = get_node('tag:spase-group.org,2018:spase://GBO')
			>>> print(cat.__class__.__name__)
			SpaseRecord

		Load a data source definition from a local file:

			>>> src = get_node('tag:place.org:2019:/hidden/my_source',
			                   'file:///home/person/my_source.json')

		Load a dataset collection from a specific remote URL:

		   >>> cat = get_node('tag:place.org:2019:/hidden/my_source',
			                   'http://place.org/catalog/my_source.json')
	"""

	sPathId = _fullName(sPathId)

	bGlobal = None
	if sUrl:
		# Here a new root is created, but the pointer to that root is lost!
		# (don't worry there's not a memory leak the C-bindings delete the node)
		#
		# Because of this we loose the C-code's caching ability for detached
		# roots.  For now I don't want to have to create any python objects that
		# hold the pointer for the underlying C node object, but maybe I should
		# do so because this would have the knock on effect of allowing for
		# multi-threaded catalog operations.
		#
		# For now sub items of a detached root will just have to be made by
		# walking the 'urls' key.  This triggers a split in the creation mode
		# for sub items of a catalog.  Children of the actual root can be made
		# using 'path' URIs, but children of detached roots have to do thier own
		# lookup via the 'urls' list.
		#
		# The result is sub-optimal and should be updated if time permits by
		# creating a C pyNode object.

		dDef = _das3.get_node(sPathId, None, sUrl)
		bGlobal = DETACHED
	else:
		dDef = _das3.get_node(sPathId)
		bGlobal = GLOBAL

	if dDef == None: return None

	if dDef['_url'] != None: sUrl = dDef['_url']

	# The node's name is the one das2C reports in '_path', which is not
	# always the text asked for: below a named seed a relative path comes
	# back as the full name, see seed().

	# Factory time:
	if dDef['type'] == 'Catalog':       return Catalog(dDef, 'catalog', FULL, bGlobal)
	if dDef['type'] == 'HttpStreamSrc': return HttpStreamSrc(dDef, FULL, bGlobal)
	if dDef['type'] == 'SourceSet':     return SourceSet(dDef, FULL, bGlobal)
	if dDef['type'] == 'FileAggSrc':
		raise NotImplementedError("FileAggSrc catalog nodes are not supported yet (%s)"%dDef.get('_url', ''))

	raise CatalogError("Data type '%s' of node from '%s' is unknown"%(
	                    dDef['type'], sUrl))

# ########################################################################### #
def node_dict(sPathId=None, sUrl=None):
	"""Get a catalog node as a plain dictionary, whatever its type.

	For looking at what a node actually says, for instance to dump it with
	json.dumps().  das2C finds and loads the node as it does for get_node(),
	but nothing here depends on the node's type, so nodes das3py has no
	class for come back too.

	Args:
		sPathId (str) : The catalog path of the node, 'site:' and 'test:'
			shorthands included.  None is the top of the catalog.

		sUrl (str, optional) : Load the node from this URL or file instead of
			finding it through the catalog, as for get_node().

	Returns:
		dict: The node's JSON content, plus two keys the node itself doesn't
		have: '_url', where it was loaded from, and '_path', its catalog
		name.

	Raises:
		_das3.Error : if the node can't be found or loaded; the message says
			where the lookup stopped and what is listed there.
	"""
	return _das3.get_node(_fullName(sPathId), None, sUrl)

# ########################################################################### #
def get_source(sPathId, sUrl=None):
	"""Read a single data source definition from a local or remote files.

	This function is essentially a wrapper around get_node() that makes sure
	the returned object has the get() method.  A path that ends at a
	dataset's set of sources, a SourceSet, gives the one das3py reads best,
	see SourceSet.source().

	Args:
		sPathId (str) : Provides the catalog Path ID for the object to load
		   Since the das2 root node at 'tag:das2.org,2012: contains the
			most commonly loaded item, any sPathId starting with 'site:/' or
			'test:/' is assumed to sub item of the das2 root node.

			Note: This value is required even if an explicit URL is provided
		      because much like files on a disk, das2 catalog objects do not
			   embed the name of node within the contents of the node.

		sUrl (str,optional) : A string providing a direct load URL.
		   URLs must start with one of 'file://', 'http://', or 'https://'.

	Returns:
		Source: An HttpStreamSrc, the only source type das3py reads so far.

	Raises:
		CatalogError : If the node is not a source, or a set with none das3py
			can read.
		_das3.error : If a low-level read error occurs.

	"""

	node = get_node(sPathId, sUrl)

	# A dataset's set of sources acts as the one das3py reads best.  The pick
	# comes back as a stub holding only the set's listing entry; load it so
	# that props has the whole node, as it does for a path that ends at the
	# source.  das2C keeps the node, so get() doesn't fetch it again.
	if isinstance(node, SourceSet):
		node = node.source()
		node.load()

	if not isinstance(node, Source):
		raise CatalogError(node.url, "Node type '%s' at %s is not a data source"%(
		                   node.__class__.__name__, node.path))
	return node

# ########################################################################### #
def get_catalog(sPathId=None, sUrl=None):
	"""Read a single directory definition from a local or remote file.

	This function is essentially a wrapper around get_node() that makes sure
	the returned object has the sub() method.

	Args: 
		sPathId (str, optional): A string providing the catalog Path ID for
			the object to load.  Since the das2 root node at 'tag:das2.org,2012:'
			contains the most commonly loaded items, any sPathId starting with
			'site:/' or 'test:/' is assumed to sub item of the das2 root node.

		sUrl (str,optional): A string providing a direct load URL.
			URLs must start with one of 'file://', 'http://', or 'https://'.

	Returs: Either a Catalog or SourceSet object is returned depending
		   on the contents of the file.

	Raises: 
		CatalogError: If the file does not describe one of these object types.
		_das3.error: If a low-level read error occurs.
	"""
	
	node = get_node(sPathId, sUrl)
	if not isinstance(node, Catalog):
		raise CatalogError(node.url, "Node type '%s' at %s is not a catalog"%(
		                   node.__class__.__name__, node.path))

	return node
	
# ########################################################################### #
#
# Need to think this through, I don't like the implication that sampling a 
# signal for different sampling periods (all above the Nyquist) produces
# different spectral densities
#
#def psd_units(time, amp):
#	"""Calculate Power Spectral Density units
#	
#	Args:
#		time (Variable, str) : The time variable used in the calculation or just
#			a units string for the sample period.
#		amp  (Variable, str) : The amplitude variable over which the power spectral
#			density was calculated or just the amplitude units string.
#	
#	Returns:
#		str : The units of a power spectral density calculation which are
#		   amp**2 / 1/period
#	"""
#	
#
#
#	sTime = time
#	if isinstance(time, Variable): 
#		sTime = time.units
#		# If I have a variable I can adjust the SI prefixes to put this in
#		# a nicer range
#		r = time.array.max()
#		
#		
#	sAmp = amp
#	if isinstance(amp, Variable):
#		sAmp = amp.units
#	
#	sFreq = _das3.unit_invert(sTime)
#	
#	sPow  = _das3.unit_pow(sAmp, 2)
#	if len(sPow) == 0:
#		return "%s-1"%sFreq
#	else:
#		return "%s %s-1"%(sPow, sFreq)
#	
#	return sSD
	
	

