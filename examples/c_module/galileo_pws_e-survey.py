#!/usr/bin/env python2

import sys
import matplotlib
matplotlib.use('Agg')  # Draw to files only.  Comment out to show plots in a window.
import matplotlib.pyplot as pyplot
import numpy as N
import das3


##############################################################################
def getArrayAndUnits(ds, sDimType, sDim, sVar):
	"""Helper function to get the numpy array and units for a variable from
	a low-level dataset dictionary.  The higher level das3 module does this
	for you: ds['frequency']['center'].array"""

	if sDimType not in ('coords','data'):
		raise ValueError("Dimension type: '"+sDimType+"', expected 'coords' or 'data")
	
	dDims = ds[sDimType]
	
	if sDim not in dDims:
		raise ValueError("%s not present among the %s dimensions of %s"%(
		                 sDim, sDimType, ds['id']))
		
	# If the center variable is not present, we could fake it with min, max,
	# width etc.  For this example just look for actual center variables.
	dVars = dDims[sDim]
	
	
	if sVar not in dVars:
		raise ValueError("Variable %s not present in %s -> %s -> %s"%(
		                 ds['id'], sDimType, sDim))
	
	# the element 'props' always contains the dimension properties such as
	# labels, etc.  It's never associated with an array
	if sVar == 'props':
		raise ValueError("'props' is list of dimension properites, not a variable")

	# Each variable names its backing array and gives its units directly.
	# The 'expression' is a summary for people, don't parse it.  Variables
	# computed from other variables have no 'array' and aren't supported in
	# this simple interface.
	dVar = dVars[sVar]
	sAry = dVar.get('array')
	sUnits = dVar['units']
	
	if sAry not in ds['arrays']:
		raise ValueError("Array '%s' not included in dataset '%s'"%(sAry, ds['id']))
		
	a = ds['arrays'][sAry]
	
	return (a, sUnits)	
	

##############################################################################

def main(argv):
	"""Annotated example of using das2C and matplotlib to gather and plot
	a day's worth of Galileo PWS electric spectrum data using the low-level
	interface.  das3py consists of a low-level C-module, _das3, and a higher
	level module that acts in a more pythonic way, das3.  Most programs
	should use the higher level module, as ex02_galileo_pws_spectra.py does
	for this same instrument.  This example shows what is underneath it.
	"""
	
	# Download a das2 Galileo PWS dynamic electric spectrum stream.  We're
	# downloading Galileo data since it's open and will not prompt for a
	# password.  
	
	# Also, to demonstrate one of the features of the library we are 
	# intentionally asking the wrong server for galileo data.  Since the server
	# planet knows where galileo data are it issues a redirect, which das2C
	# follows.
	sUrl = 'http://planet.physics.uiowa.edu/das/das2Server?server=dataset&'+\
	       'dataset=Galileo/PWS/Survey_Electric&start_time=2001-001&' +\
	       'end_time=2001-002&resolution=60'
	
	print("Reading Galileo PWS spectra for 2001-001 at intrinsic resolution")
	(dHdr, lDs) = das3._das3.read_server(sUrl)
	print("%d correlated dataset(s) downloaded\n"%len(lDs))
	
	# So what did we get?  The low-level interface just returns a list of
	# dataset objects, which are just dictionaries.  The higher level das3
	# module wraps these in python classes, here we interact with the
	# low-level objects directly.
	#
	# The low-level output always has the same morphology:
	# 
	# list_of_datasets (1)
	# |
	# |- Dataset (1 - N)
	#   |
	#   |- Properties  (0 - N)
	#   |
	#   |- Arrays      (1 - N)
	#   |
	#   |- Dimensions  (1 - N)
	#       |
	#       |- Properties (0 - N)
	#       |
	#       |- Variables  (1 - N)
	#
	# If a person knows what they want, they can skip the Dimension and 
	# Variable objects all together, but they are needed to support machine-
	# readable understanding of the dataset as Variables define lookup
	# functions for arrays that may include uniary and binary operation
	# trees in addition to index maps.
	
	
	# The server sends one dataset per frequency band of the instrument, each
	# with its own frequency table and its own time resolution.  The first
	# is used below to show what a dataset holds, and all of them are
	# plotted at the end.
	ds = lDs[0]
	
	# The info element contains a string overview of the dataset
	print(ds['info'])
	
	# The rank element contains the overall iteration rank of the dataset
	# note that unlike ISTP-CDF this has nothing to do with the number of
	# physical dimensions spanned by the values.  Though we won't be working 
	# with Variables directly in this example, all Variablse in a Dataset
	# take the same set of indicies.  Typically time values are degenerate in
	# the second index and frequency values are degenerate in the first index.
	# Data values typically are not degenerate.
	print("Iteration rank is: %s"%ds['rank'])
	
	# Let's list the physical coordinate dimensions of the dataset
	sCoords = ', '.join(ds['coords'].keys())
	print("Data are located in physical dimensions:", sCoords)
	
	# Let's list the physical data dimensions of the dataset
	sData = ', '.join(ds['data'].keys())
	print("Data are defined in the physical dimensions:", sData)
	
	# Each physical dimension has 1-N variables that locate values in that
	# dimension.  Each variable that happens to be based on an array 
	# (virtual variables exist as well and are important for reducing
	# network overhead) provides a mapping between overall dataset indices
	# to array indices.  The purpose (or role) of each variable is defined
	# by it's name.  Other names are possible, but here are the built-in
	# Variable names that higher level code will know how to interpret:
	#
	#  center - Absolute point in a dimension, center of bin 
	#
	#  min    - Absolute point in a dimension, start of bin
	#
	#  max    - Absolute point in a dimension, end of bin
	#
	#  width  - Difference value in a dimesion, width of bin
	#
	#  mean   - Absolute point in a dimension, average value in a bin
	#
	#  mode   - Absolute point in a dimension, most frequent value in a bin
	#
	#  reference - Same as 'min' but is usually paired with 'offset'
	#
	#  offset - Difference value in dimenision, to be added to reference,
	#            very handy for waveforms where start time and offsets
	#            should be denoted separately for automatic power spectral
	#            density calculations.
	#
	#  max_error - Absolute point in a dimension, denotes uncertianty
	#
	#  min_error - Absolute point in a dimension, denotes uncertianty 
	#
	#  uncertianty - Difference value in a dimension, denotes uncertianty
	#            assumed to be centered on 'center'
	#
	#  std_dev - Standard deviation of values in a bin, does *not* have
	#            the same absolute or difference units as the center
	#            values.
	#
	#  point_spread - A sub-iteration array of difference values from
	#            a reference point.
	#
	#  weight - An sub-iteration array of weights for a point_spread
	#   
	# Not all of these are supplied in each dateset, in fact most of 
	# them aren't.  Typically only the center Variable is supplied.
	
	# Lets get the data arrays for the center Variables and totally
	# ignore the index map.  One (time, frequency, spectral density) set
	# per dataset.
	lBands = []
	for ds in lDs:
		(aY, unitsY) = getArrayAndUnits(ds, 'coords', 'frequency', 'center')
		(aZ, unitsZ) = getArrayAndUnits(ds, 'data',  'electric', 'center')

		# Times are converted on read to numpy datetime64[ns] (units ns1970),
		# which matplotlib plots directly as dates
		(aX, unitsX) = getArrayAndUnits(ds, 'coords', 'time', 'center')

		# The stream marks missing measurements with the fill value 0, which
		# has no logarithm.  Mask those so they are left blank.  N.ma.log10
		# skips masked values, plain N.log10 would compute them and warn.
		aZ = N.ma.log10(N.ma.masked_less_equal(aZ, 0).transpose())
		lBands.append( (aX, N.log10(aY), aZ) )

	# One color scale for all the bands, or the same color would mean a
	# different spectral density in each
	rMinZ = min([aZ.min() for (aX, aY, aZ) in lBands])
	rMaxZ = max([aZ.max() for (aX, aY, aZ) in lBands])

	for (aX, aY, aZ) in lBands:
		pyplot.pcolormesh(aX, aY, aZ, cmap='jet', vmin=rMinZ, vmax=rMaxZ)

	xMin = min([aX.min() for (aX, aY, aZ) in lBands])
	xMax = max([aX.max() for (aX, aY, aZ) in lBands])
	yMin = min([aY.min() for (aX, aY, aZ) in lBands])
	yMax = max([aY.max() for (aX, aY, aZ) in lBands])
	pyplot.axis([xMin, xMax, yMin, yMax])

	cbar = pyplot.colorbar()
	cbar.set_label("log(%s)"%unitsZ)
	pyplot.gcf().autofmt_xdate()  # Slant the dates so they don't overlap
	pyplot.xlabel("%s to %s UTC"%(
		N.datetime_as_string(xMin, unit='m'), N.datetime_as_string(xMax, unit='m')
	))
	pyplot.ylabel("log(%s)"%unitsY)
	pyplot.title(dHdr['props']['title'][1])
	pyplot.savefig('galileo_pws_e-survey.png')  # Or pyplot.show(), with the backend line above commented out


if __name__ == '__main__':
	sys.exit(main(sys.argv))
