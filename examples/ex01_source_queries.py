# das3 example 01:  Data queries using catalog Source nodes
#
# The lowest level nodes in a das2 catalog are usually Source nodes which
# describe where and how to get data.  Without source nodes the catalog
# wouldn't be very useful.  The most common source node type is HttpStreamSrc,
# which can describe many HTTP GET based data service including das2 servers
# and HAPI servers.  A future Source derived class will provide access to
# static file collections.
#
# There are two ways to ask for data, and they don't mix.
#
#  1.  Through the source's interface, Source.get()
#
#      The source node describes what can be asked for: coordinate ranges,
#      which data to send, reader options.  das2C checks a query against
#      that description and translates it into the request the server
#      takes.  When a server is replaced or its parameters change the node
#      is updated and queries written this way keep working.  This is the
#      route to use.
#
#      Every source also provides at least one example, a complete query
#      that is known to return data.  get() with no arguments runs one.
#
#  2.  By URL, das3.read_http()
#
#      Any URL that returns a das stream can be read directly.  Nothing is
#      checked, the catalog isn't involved, and the code is tied to one
#      server's parameters.  It's there for servers that have no catalog
#      entry and for testing.

import das3
import sys


sId = 'site:/uiowa/cassini/rpws/hires_midfreq_waveform'

# 1a. Getting example data.  All das catalog Source definitions are
#     required to provide at least one example dataset.

src = das3.get_source(sId)
for number, label in src.examples(): print(number, "|", label)
streamHdr, lDatasets = src.get()  # The first example, if no query is given


# 1b. An interface query.  info() lists what can be asked for; its Key
#     column holds the names a query uses.

print(src.info())

# A tuple is a range.  This source's 'rate' option selects between the
# receiver's 10 kHz and 80 kHz bands.
dQuery = { 'time':('2008-223T09:06', '2008-223T09:13'), 'rate':'80khz' }
streamHdr, lDatasets = src.get(dQuery, verbose=True)

# A query das2C can't make sense of is refused before anything is sent, with
# the reason for each bad entry.
try:
   src.get({ 'time':('2008-223T09:06', '2008-223T09:13'), 'rate':'40khz' })
except ValueError as e:
   print(e)


# 2. A request by URL (discouraged, but sometimes necessary).  infoProto()
#    shows the GET parameters the server behind this source takes, and
#    baseUrls() where it is.  If the data move to a server with different
#    parameters this code breaks, and the query in 1b does not.  That
#    happens: in 2026 many das2 servers were replaced by dasflex, which
#    spells the time range read.time.min and read.time.max.

print(src.infoProto())

sBase = src.baseUrls()[0]
sUrl = sBase + '&start_time=2008-223T09:06&end_time=2008-223T09:13&params=--80khz'
streamHdr, lDatasets = das3.read_http(sUrl)

# Print dataset info. Actually using the datasets returned via these queries
# is the subject of subsequent examples

for ds in lDatasets:
   print(ds)
   print("")
