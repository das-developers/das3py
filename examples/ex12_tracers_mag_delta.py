# das3py example 12:  TRACERS-2 magnetometer delta-B over a southern ROI
#
# The first TRACERS example, and the first to use each part of das3 at once:
#
#  1. The das catalog.  The path below ends at a SourceSet, the list of
#     places one dataset can be had from.  get_source() picks the one das3py
#     reads best, here a das3 stream server.  das2C reads the source's
#     definition and builds the request, so this script never sees the
#     server's own GET parameters.  print(src.info()) shows what can be set.
#
#  2. A das3 stream.  The server sends binary das3 by default.
#
#  3. A composite variable.  The three components of delta-B arrive as one
#     variable whose values are 3-vectors.  Its array has the shape
#     (records, 3): the first index is the dataset's, the second is internal
#     to each vector.  The variable says what the vectors are (.ops), names
#     the components (.labels) and splits the shape in two (.extShape() and
#     .intrShape()).
#
# The plot puts the three components on one axis, in red, green and blue.
# It covers one region of interest (ROI), a few minutes when the spacecraft
# crosses the auroral zone and the field-aligned currents there.

import sys
import das3
import das3.mpl

import matplotlib
matplotlib.use('Agg')  # Draw to files only.  Comment out to show plots in a window.
import matplotlib.pyplot as pyplot
import matplotlib.dates  as dates

# The southern ROI of TRACERS-2 orbit 4241, TS2_ROI_04241_1 in the TRACERS
# SOC's ROI list (ts2_roi-list.csv), at geographic latitudes -79 to -51
# degrees.  The whole orbit, 2026-05-04T01:16:08 to 02:52:38 by the SOC's
# orbit table, is about 91,000 points at 16 per second; the ROI is 7,800.
ORBIT = 4241
BEG   = '2026-05-04T01:29:59'
END   = '2026-05-04T01:38:07'

src = das3.get_source('site:/uiowa/tracers/tracers-2/l2/mag_16sps_delta')

# The source offers delta-B in six frames, each a data variable that can be
# switched on or off.  Field-aligned coordinates (FAC) are on by default and
# named here only to show how; try ts2_l2_mag_16sps_gei2000_deltab instead.
# The time range is a (begin, end) tuple, a single time such as
# '2026-05-04' would mean that whole day.
sVar = 'ts2_l2_mag_16sps_fac_deltab'
(hdr, lDs) = src.get({'time':(BEG, END), sVar:True})

ds = lDs[0]
print(ds)

dim  = ds[sVar]
dB   = dim['center']
time = ds['time']['center']

print("Formalism: %s"%dB.ops)
print("Components: %s, internal shape %s, external shape %s"%(
	dB.labels, dB.intrShape(), dB.extShape()
))

fig, ax = pyplot.subplots(figsize=(11, 5))

for (iComp, sColor) in enumerate(('red', 'green', 'blue')):
	ax.plot(
		time.array, dB.array[:, iComp], color=sColor, linewidth=0.6,
		label=dB.labels[iComp]
	)

ax.legend(loc='upper right')
ax.axhline(0.0, color='gray', linewidth=0.5)
ax.set_ylabel("%s (%s)"%(das3.mpl.label(dim.props['label']), dB.units))
ax.set_title("TRACERS-2 MAG, delta-B from IGRF, %s frame, orbit %d southern ROI"%(
	dB.ops['frame'], ORBIT
))

ax.xaxis.set_major_formatter(dates.DateFormatter("%H:%M:%S"))
ax.fmt_xdata = dates.DateFormatter("%Y-%m-%dT%H:%M:%S")  # mouse over dates
ax.set_xlabel(das3.mpl.range_label(ds['time']))
ax.set_xlim(time.array[0], time.array[-1])

pyplot.savefig('ex12_tracers_mag_delta.png', dpi=100)  # Or pyplot.show(), with the backend line above commented out
