/* Copyright (C) 2015-2026 Chris Piker <chris-piker@uiowa.edu>
 *
 * This file is part of das3py, the das2C python wrapper
 * 
 * Das3py is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *
 * Das3py is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * version 2.1 along with das3py; if not, see <http://www.gnu.org/licenses/>. 
 */

#include <Python.h>
#include <math.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

/*#ifdef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
*/

/* Python 2 doesn't have the Py_TYPE function but Python 3 does */
#ifndef Py_TYPE
#define Py_TYPE(ob) (((PyObject*)(ob))->ob_type)
#endif


#include <das3/util.h>
#include <das3/units.h>
#include <das3/time.h>
#include <das3/log.h>
#include <das3/units.h>
#include <das3/creds.h>
#include <das3/oper.h>
/* #include <python3.4m/modsupport.h> */

/* static char* myname i= "_das3"; */

/* ************************************************************************* */
/* The exceptions and logging */

#define HAS_DAS2_PYEXCEPT
static PyObject* g_pPyD2Error;
static PyObject* g_pPyD2QueryErr;
static PyObject* g_pPyD2AuthErr;

/* das2C keeps no copy of an error after daserrf() returns, so the log hook
 * below saves the most recent one here, location included, for
 * pyd2_setException().  The strings in a DasLogMsg die when the hook
 * returns, hence the copies. */
#define D2PY_ERR_LOC_SZ 256
typedef struct pyd2_err {
	das_errcode nErr;                 /* 0 when nothing saved */
	char sMsg[DASLOG_MSG_SZ];
	char sFile[D2PY_ERR_LOC_SZ];
	char sFunc[D2PY_ERR_LOC_SZ];
	int  nLine;
} pyd2_err_t;

static pyd2_err_t g_lastErr = {0};

/* Raise pExcept from the last daserrf() record, then forget it.  Without the
 * reset, a later call that fails without logging would report an old,
 * unrelated error as its own. */
static PyObject* pyd2_setException(PyObject* pExcept){
	if(g_lastErr.nErr == DAS_OKAY){
		PyErr_SetString(pExcept, "das2C reported failure but logged no error");
		return NULL;
	}
	PyErr_Format(
		pExcept, "%s (reported from %s:%d, %s)", g_lastErr.sMsg, g_lastErr.sFile,
		g_lastErr.nLine, g_lastErr.sFunc
	);
	memset(&g_lastErr, 0, sizeof(g_lastErr));
	return NULL;
}

/* Same, but the bare message, which is what the dft objects have always
 * raised.  A saved record with a code other than nErr belongs to some other
 * failure; DAS_OKAY for nErr takes any record. */
static void pyd2_setErrText(PyObject* pExcept, das_errcode nErr){
	if((g_lastErr.nErr != DAS_OKAY)&&((nErr == DAS_OKAY)||(nErr == g_lastErr.nErr)))
		PyErr_SetString(pExcept, g_lastErr.sMsg);
	else
		PyErr_SetString(pExcept, "Unknown error");
	memset(&g_lastErr, 0, sizeof(g_lastErr));
}

/* Keep errors so that they may be returned as exceptions, let INFO go to
 * stderr and deep six DEBUG and below */
#define D2PY_LOG_BUF_SZ DASLOG_MSG_SZ
static char g_sLogBuf[D2PY_LOG_BUF_SZ] = {'\0'};

/* Runs under das2C's log lock, on whatever thread logged.  Per the hook
 * contract in log.h it must not call back into das2C or take the GIL, so
 * it only copies text into C buffers. */
static void pyd2_onLog(const DasLogMsg* pMsg, void* pUser)
{
	(void)pUser;

	if(pMsg->errCode != DAS_OKAY){
		g_lastErr.nErr = pMsg->errCode;
		snprintf(g_lastErr.sMsg, DASLOG_MSG_SZ, "%s", pMsg->msg);
		snprintf(g_lastErr.sFile, D2PY_ERR_LOC_SZ, "%s", pMsg->file ? pMsg->file : "?");
		snprintf(g_lastErr.sFunc, D2PY_ERR_LOC_SZ, "%s", pMsg->func ? pMsg->func : "?");
		g_lastErr.nLine = pMsg->line;
	}

	if(pMsg->level >= DASLOG_WARN){
		/* Copy the error message into the buffer if and only if it's empty */
		/* this way later errors don't over-write the root cause */
		if(g_sLogBuf[0] == '\0') snprintf(g_sLogBuf, D2PY_LOG_BUF_SZ, "%s", pMsg->msg);
		return;
	}

	if(pMsg->level == DASLOG_INFO){
		fprintf(stderr, "INFO: %s\n", pMsg->msg);
	}
}

/* Set and exception and clean the log buffer */
static PyObject* pyd2_setExceptFromLog(PyObject* pExcept){
	
	if(g_sLogBuf[0] != '\0'){
		PyErr_SetString(pExcept, g_sLogBuf);
		memset(g_sLogBuf, 0, D2PY_LOG_BUF_SZ);
	}
	else
		PyErr_SetString(pExcept, 
			"Uh Oh! :o\nYou've encountered an unlogged error in das2C, this "
			"shouldn't happen.  Please contact das-developers @ uiowa.edu and "
			"let them know about the problem along with any steps that may be "
			"taken to reproduce the bug.\n"
			"Thanks a lot for your help, we appreciate it! :) ");
	return NULL;
}


/* DasUnits_fromStr() still makes a unit out of any well formed string,
 * "cows^2" included, but returns NULL for broken syntax such as "cows^"
 * where v2 took it as is.  Raise for that here, before NULL reaches a das2C
 * call that treats it as an error of its own. */
static bool _pyd2_units(const char* sUnits, DasUnits* pUnits){
	*pUnits = DasUnits_fromStr(sUnits);
	if(*pUnits != NULL) return true;
	PyErr_Format(PyExc_ValueError, "Can't parse units '%s'", sUnits);
	return false;
}

/*****************************************************************************/
/* parsetime */

const char pyd2help_parsetime[] = 
"parsetime(sDateTime)\n"
"\n"
"Converts most human-parseable time strings to numeric components.\n"
"\n"
"This function has no concept of leap seconds.  So the maximum number of\n"
"seconds in a minute is 60, not 61 or 62.\n"
"\n"
"Args:\n"
"   sDateTime (str) : A string to parse to a date\n"
"\n"
"Returns:\n"
"   A 7-tuple containing the broken down time values\n"
"\n"
"   - **year** (*int*): The (typically) 4-digit year value\n"
"   - **month** (*int*): The month of year starting with 1\n"
"   - **mday** (*int*): The day of the month from 1 to 31\n"
"   - **yday** (*int*): The day of the year from 1 to 366\n"
"   - **hour** (*int*): The hour of the day from 0 to 23\n"
"   - **minute** (*int*): The minute of the hour from 0 to 59\n"
"   - **seconds** (*float*): The seconds of the minute from 0.0 to < 60.0\n"
"\n"
"Raises:\n"
"   ValueError: If the time is not parsable\n"
"\n";

static PyObject* pyd2_parsetime(PyObject* self, PyObject* args)
{
	const char* sTime;
	DasTime dt;
	
	if(!PyArg_ParseTuple(args, "s:parsetime", &sTime))
		return NULL;
	
	if(!DasTime_fromStr(&dt, sTime)){
		PyErr_SetString(PyExc_ValueError, "String was not parsable as a datetime");
		return NULL;
	}
	
	return Py_BuildValue("(iiiiiid)", dt.year, dt.month, dt.mday, dt.yday,
		dt.hour, dt.minute, dt.second);
}

const char pyd2help_parse_epoch[] = 
"parse_epoch(rTime, sUnits)\n"
"\n"
"Converts and floating point das2 epoch time into a numeric calendar\n"
"components.\n"
"\n"
"Args:\n"
"   rTime (float) : A floating point time value whose units are and reference\n"
"      point are determined by `sUnits` argument\n"
"\n"
"   sUnits (str) : One of the das2 timestamp units types as defined in `units.c`\n"
"      in the underlying libdas2 C library.  Known unit types are:\n"
"\n"
"      - **'us2000'** : Microseconds since midnight, January 1st 2000\n"
"      - **'mj1958'** : Days since midnight, January 1st 1958\n"
"      - **'t2000'**  : Seconds since midnight, January 1st 2000\n"
"      - **'t1970'**  : Seconds since midnight, January 1st 1970, commonly called the UNIX epoch.\n"
"      - **'ns1970'** : Nanoseconds since midnight, January 1st 1970, commonly used by numpy.\n"
"      - **'TT2000'** : Nanoseconds since 2000-01-01T11:58:55.816 includes leap seconds\n"
"\n"
"Note: Only the TT2000 range includes leapseconds.  All others ignore\n"
"      leapseconds as if they did not occur."
"\n"
"Returns:\n"
"   A 7-tuple containing the broken down time values\n"
"\n"
"   - **year** (*int*): The (typically) 4-digit year value\n"
"   - **month** (*int*): The month of year starting with 1\n"
"   - **mday** (*int*): The day of the month from 1 to 31\n"
"   - **yday** (*int*): The day of the year from 1 to 366\n"
"   - **hour** (*int*): The hour of the day from 0 to 23\n"
"   - **minute** (*int*): The minute of the hour from 0 to 59\n"
"   - **seconds** (*float*): The seconds of the minute from 0.0 to < 60.0*\n"
"\n"
" *Except for TT2000 conversions, where seconds are 0.0 to < 61.0 at times\n"
"\n"
"Raises:\n"
"   ValueError: If `sUnits` is an unknown time value format\n"
"\n";
static PyObject* pyd2_parse_epoch(PyObject* self, PyObject* args)
{
	double rTime = 0.0;
	const char* sUnits = NULL;
	
	if(!PyArg_ParseTuple(args, "ds:parse_epoch", &rTime, &sUnits))
		return NULL;
	
	DasUnits units;
	if(!_pyd2_units(sUnits, &units)) return NULL;
	if(! DasUnits_isTimePoint(units)){
		PyErr_SetString(PyExc_ValueError, "Units are not a recognized epoch time");
		return NULL;
	}
	
	DasTime dt;
	DasTime_fromPoint(&dt, rTime, units);
	
	return Py_BuildValue("(iiiiiid)", dt.year, dt.month, dt.mday, dt.yday, 
	                     dt.hour, dt.minute, dt.second);
}

const char pyd2help_to_epoch[] = 
"to_epoch(nYr, nMon, nDom, nHr, nMin, rSec, sUnits)\n"
"\n"
"Encodes a broken down time as an floating point value in the given time\n"
"offset units.  The units define both the epoch and interval.  Arguments\n"
"will be normalized if necessary.\n"
"\n"
"Args:\n"
"   sUnits (str) - The time encoding, should be one of \n"
"   nYr  (int)          - year\n"
"   nMon (int,optional) - month of year (1-12)\n"
"   nDom (int,optional) - day of month (1-31)\n"
"   nHr  (int,optional) - hour of day (0-23)\n"
"   nMin (int,optional) - minute of hour (0-59)\n"
"   rSec (float,optional) - second of minute (0.0 <= s < 60.0)*\n"
"\n"
"Returns (float):\n"
"   The encoded floating point epoch time.\n"
"\n"
"Raises:\n"
"   ValueError: If `sUnits` is an unknown time value format\n"
"\n"
"Note:  To use day of year as input, simple specify 1 for the month and\n"
"the day of year in place of day of month. ONLY the day of month and\n"
"higher fields are normalized!\n"
"\n"
"TT2000 Note: If the output units are TT2000, the seconds field can be\n"
"greater then 60.0.\n"
"\n";

static const int days[2][14] = {
  { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
  { 0, 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 } };

static PyObject* pyd2_to_epoch(PyObject* self, PyObject* args)
{
	//             y  m  md yd h  m  s
	DasTime dt = {0, 1, 1, 1, 0, 0, 0.0};
	double rEpoch = 0.0;
	const char* sTo = NULL;

	if(!PyArg_ParseTuple(
		args, "si|iiiid:to_epoch", &sTo, &(dt.year), &(dt.month), 
		&(dt.mday), &(dt.hour), &(dt.minute), &(dt.second)
	)) return NULL;

	DasUnits units;
	if(!_pyd2_units(sTo, &units)) return NULL;
	if(! DasUnits_isTimePoint(units)){
		PyErr_SetString(PyExc_ValueError, "Units are not a recognized epoch time");
		return NULL;
	}

   /* Adapted from time.c ................................................... */

#define LEAP(y) ((y) % 4 ? 0 : ((y) % 100 ? 1 : ((y) % 400 ? 0 : 1)))

	int leap, ndays;

	/* month is required input -- first adjust month */
	if (dt.month > 12 || dt.month < 1) {
		/* temporarily make month zero-based */
		(dt.month)--;
		dt.year += dt.month / 12;
		dt.month %= 12;
		if (dt.month < 0) {
			dt.month += 12;
			(dt.year)--;
		}
		(dt.month)++;
	}

	/* index for leap year */
	leap = LEAP(dt.year);

	/* day-of-year is output only -- calculate it */
	dt.yday = days[leap][dt.month] + dt.mday;

	/* final adjustments for year and day of year */
	ndays = leap ? 366 : 365;
	if (dt.yday > ndays || dt.yday < 1) {
		while (dt.yday > ndays) {
			(dt.year)++;
			dt.yday -= ndays;
			leap = LEAP(dt.year);
			ndays = leap ? 366 : 365;
		}
		while (dt.yday < 1) {
			(dt.year)--;
			leap = LEAP(dt.year);
			ndays = leap ? 366 : 365;
			dt.yday += ndays;
		}
	}

	/* and finally convert day-of-year back to month and day */
	while (dt.yday <= days[leap][dt.month]) (dt.month)--;
	while (dt.yday >  days[leap][dt.month + 1]) (dt.month)++;
	dt.mday = dt.yday - days[leap][dt.month];

#undef LEAP
	/* ........................................... end adapted from time.c */

	/* Okay, we can now safely convert day-of-year times to TT2000 since
	   we won't accidentally roll-over the leap seconds */
	rEpoch = DasTime_toPoint(&dt, units);
	return Py_BuildValue("d", rEpoch);
}

const char pyd2help_tt2k_utc[] = 
  "Special conversion from TT2000 integers without floating point round off\n"
  "\n"
  "Args:"
  "   nTT2000 - a long integer (just int in python3)\n"
  "\n"
  "Returns: (nYear, nMonth, nDom, nHour, nMinute, dSeconds)\n"
  "         which is suitable as a DasTime constructor value\n";

static PyObject* pyd2_tt2k_utc(PyObject* self, PyObject* args)
{
	long long  tt = 0LL; 
	if(!PyArg_ParseTuple(args, "L:tt2k_utc", &tt))
		return NULL;

	int iyr, imt, idy, ihr, imn, isc;
	int32_t nNano;

	/* Integer fields straight from das2C, the only rounding is the final
	   add, same as the v2 CDF call path did */
	DasTime_tt2kToUtc((int64_t)tt, &iyr, &imt, &idy, &ihr, &imn, &isc, &nNano);
	double dSec = isc + nNano*1e-9;
	
	return Py_BuildValue("(iiiiid)", iyr, imt, idy, ihr, imn, dSec);
}

const char pyd2help_utc_tt2k[] = 
  "Special conversion to TT2000 integers without floating point round off\n"
  "\n"
  "Args:\n"
  "   nYear, nMonth, nDom, nHour, nMinute, dSeconds\n"
  "\n"
  "Note that if 60.0 is allow for seconds if this is a leap second\n"
  "\n"
  "Returns (int) - A TT2000 value good to nanoseconds\n";

static PyObject* pyd2_utc_tt2k(PyObject* self, PyObject* args)
{
	int nYr, nMn, nDom, nHr, nMin;
	double dSec;

	if(!PyArg_ParseTuple(args, "iiiiid:utc_tt2k", &nYr, &nMn, &nDom, &nHr, &nMin, &dSec))
		return NULL;
		
	int nSc = (int)dSec;           /* seconds field */

	/* One rounding to the nanosecond.  A fraction that rounds up to a whole
	   second is fine as is, das2C carries nanosec past 1e9 into the second
	   field, leap second aware. */
	int32_t nNano = (int32_t)llround((dSec - nSc)*1e9);

	/* v2 returned the CDF illegal-value sentinel for out of range fields,
	   which nothing checked; das2C now reports them, so raise instead */
	int64_t ntt2k = 0;
	if(DasTime_utcToTt2k(nYr, nMn, nDom, nHr, nMin, nSc, nNano, &ntt2k) != DAS_OKAY)
		return pyd2_setException(PyExc_ValueError);

	return Py_BuildValue("L", (long long)ntt2k);
}


const char pyd2help_ttime[] = 
  "Converts time components to a double precision floating point value\n"
  "(seconds since the beginning of 1958, ignoring leap seconds) and\n"
  "normalize inputs.  Note that this floating point value should only be \n"
  "used for \"internal\" purposes.  (There's no need to propagate yet\n"
  "another time system, plus I want to be able to change/fix these values.)\n"
  "\n"
  "There is no accomodation for calendar adjustments, for example the\n"
  "transition from Julian to Gregorian calendar, so I wouldn't recommend\n"
  "using this routine for times prior to the 1800's.\n"
  "\n"
  "Arguments (will be normalized if necessary):\n"
  "   int year                - year (1900 will be added to two-digit values)\n"
  "   int month (optional)    - month of year (1-12)\n"
  "   int mday (optional)     - day of month (1-31)\n"
  "   int hour (optional)     - hour of day (0-23)\n"
  "   int minute (optional)   - minute of hour (0-59)\n"
  "   float second (optional) - second of minute (0.0 <= s < 60.0), \n"
  "                             leapseconds ignored\n"
  "\n"
  "Note:  To use day of year as input, simple specify 1 for the month and\n"
  "the day of year in place of day of month.  Beware of the normalization.\n";
  
static PyObject* pyd2_ttime(PyObject* self, PyObject* args)
{
	int year;
	int month = 1;
	int mday = 1;
	int hour = 0;
	int min = 0;
	double sec = 0.0;
	double dRet;
	
	if(!PyArg_ParseTuple(args, "i|iiiid:ttime", &year, &month, &mday, &hour,
			               &min, &sec))
		return NULL;
	
	DasTime dt = {year, month, mday, 0, hour, min, sec};
	dRet = DasTime_toSec1958(&dt);
	
	return Py_BuildValue("d", dRet);
}

const char pyd2help_emitt[] = 
	"Performs the inverse operation as ttime.  Converts floating point\n"
	"seconds since the beginning of 1958 back into a broken down time \n"
	"tuple:\n"
	"\n"
	"  (year, month, mday, yday, hour, minute, float_seconds)\n";

static PyObject* pyd2_emitt(PyObject* self, PyObject* args)
{
	double dEpoch;
	DasTime dt;

	if(!PyArg_ParseTuple(args, "d:emitt", &dEpoch))
		return NULL;

	DasTime_fromSec1958(&dt, dEpoch);
	
	return Py_BuildValue("(iiiiiid)", dt.year, dt.month, dt.mday, dt.yday,
		dt.hour, dt.minute, dt.second);
}

const char pyd2help_tnorm[] =
	"Normalizes date and time components\n"
	"Arguments (will be normalized if necessary):\n"
	"   int year                - year (1900 will be added to two-digit values)\n"
	"   int month (optional)    - month of year (1-12)\n"
	"   int mday (optional)     - day of month (1-31)\n"
	"   int hour (optional)     - hour of day (0-23)\n"
	"   int minute (optional)   - minute of hour (0-59)\n"
	"   float second (optional) - second of minute (0.0 <= s < 60.0), \n"
	"                             leapseconds ignored\n"
	"\n"
	"Note:  To use day of year as input, simple specify 1 for the month and\n"
	"the day of year in place of day of month.  Beware of the normalization.\n"
	"Returns a tuple of the form:\n"
	"\n"
	"   (year, month, mday, yday, hour, minute, float_seconds)\n";

static PyObject* pyd2_tnorm(PyObject* self, PyObject* args)
{
	int year;
	int month = 0;
	int mday = 0;
	int yday = 0;
	int hour = 0;
	int min = 0;
	double sec = 0.0;

	if(!PyArg_ParseTuple(args, "i|iiiid:tnorm", &year, &month, &mday,
						 &hour, &min, &sec))
		return NULL;

	DasTime dt = {year, month, mday, yday, hour, min, sec};
	DasTime_normalize(&dt);

	return Py_BuildValue("(iiiiiid)", dt.year, dt.month, dt.mday, dt.yday,
		dt.hour, dt.minute, dt.second);
}

/* ************************************************************************* */
/* Unit conversions */


static const char pyd2help_unit_norm [] = 
"Normalize arbitrary unit strings to a standard compact form.\n"
"The output of this function was inspired by the PDS3 Units rules\n"
"\n"
"Args:\n"
"   long_units (str) : The given units\n"
"Returns:\n"
"   Compact unit representation string.  Note this string does *not*\n"
"   follow the unconventional PDS4 unit representation rules\n"
"\n";

static PyObject* pyd2_unit_norm(PyObject* self, PyObject* args){
	const char* sFrom = NULL;

	if(!PyArg_ParseTuple(args, "s:unit_norm", &sFrom)) return NULL;
	
	DasUnits to;
	if(!_pyd2_units(sFrom, &to)) return NULL;
	const char* sTo = DasUnits_toStr(to);
	return Py_BuildValue("s", sTo);
}

static const char pyd2help_convertible [] = 
"Determine if units are interchangeable.\n"
"\n"
"This function is not as complete a solution as using UDUNITS2 but should\n"
"work quite well for common space physics quantities as well as SI units.\n"
"Units are convertible if:\n"
"   #- They are both known time offset units.\n"
"   #- They have a built in conversion factor (ex: 1 day = 24 hours)\n"
"   #- Both unit sets use SI units, including Hz\n"
"   #- When reduced to base units the exponents of each unit are the same.\n"
"\n"
"Args:\n"
"  fromUnits (str) : The given units\n"
"  toUnits   (str) : The desired units\n"
"Returns:\n"
"  True if there exists a linear relationship between values expressed in the\n"
"  two unit sets.  Said another way, if there exists an equation:\n"
"     TO_VALUE = M * FROM_VALUE + B\n"
"  where M and A are constants, then the units are convertible and this\n"
"  function should return true.\n";

static PyObject* pyd2_convertible(PyObject* self, PyObject* args){
	const char* sFrom = NULL;
	const char* sTo = NULL;
	
	if(!PyArg_ParseTuple(args, "ss:convertible", &sFrom, &sTo)) return NULL;
	
	/* Unparsable is an error, not "not convertible", so a typo such as
	 * 'tt2000' for 'TT2000' isn't silently answered False */
	DasUnits from, to;
	if(!_pyd2_units(sFrom, &from) || !_pyd2_units(sTo, &to)) return NULL;

	if(DasUnits_canConvert(from, to))  Py_RETURN_TRUE;
	else Py_RETURN_FALSE;
}

static const char pyd2help_convert[] = 
"Convert a value in one set of units to another.\n"
"\n"
"For pure interval units (seconds, meters, etc.) that have on implied zero\n"
"point, this function can be used to get a conversion factor between units\n"
"by setting fromVal to 1.0.  See also :meth:`_das3.convertible`\n"
"\n"
"Args:\n"
"  fromVal (float) : The original value, can be set to 1.0 to get the\n"
"                    conversion factor F, in: toVal = F * fromVal \n"
"  fromUnits (str) : The original units for the value\n"
"  toUnits (str)   : The new units for the value\n"
"\n"
"Returns:\n"
"  A floating point value in the desired units.\n";

static PyObject* pyd2_convert(PyObject* self, PyObject* args)
{
	const char* sFrom = NULL;
	const char* sTo = NULL;
	double rFrom = 0.0;
	if(!PyArg_ParseTuple(args, "dss:convert", &rFrom, &sFrom, &sTo)) return NULL;
	
	DasUnits from, to;
	if(!_pyd2_units(sFrom, &from) || !_pyd2_units(sTo, &to)) return NULL;
	
	double rTo = DasUnits_convert(rFrom, from, to);
	return Py_BuildValue("d", rTo);
}

static const char pyd2help_unit_mul[] = 
"Combine unit sets via multiplication\n"
"\n"
"Args:\n"
"  left_units (str) : The left side units\n"
"  right_units (str) : The right side units\n"
"\n"
"Returns:\n"
"  A new units string\n";

static PyObject* pyd2_unit_mul(PyObject* self, PyObject* args)
{
	const char* sLeft = NULL;
	const char* sRight = NULL;
	if(!PyArg_ParseTuple(args, "ss:unit_mul", &sLeft, &sRight)) return NULL;
	
	DasUnits left;
	if(!_pyd2_units(sLeft, &left)) return NULL;
	DasUnits right;
	if(!_pyd2_units(sRight, &right)) return NULL;
	
	bool bRet = DasUnits_canMerge(left, DASOP_MUL, right);
	if(!bRet){
		PyErr_Format(PyExc_TypeError, "Unsupported operation '*' for units "
				       "%s and %s", sLeft, sRight);
		return NULL;
	}
	
	DasUnits ret = DasUnits_mul(left, right);
	const char* sTo = DasUnits_toStr(ret);
	return Py_BuildValue("s", sTo);
}

static const char pyd2help_unit_div[] = 
"Combine unit sets via division\n"
"\n"
"Args:\n"
"  left_units (str) : The numerator units\n"
"  right_units (str) : The denominator units\n"
"\n"
"Returns:\n"
"  A new units string\n";

static PyObject* pyd2_unit_div(PyObject* self, PyObject* args)
{
	const char* sNum = NULL;
	const char* sDenom = NULL;
	if(!PyArg_ParseTuple(args, "ss:unit_div", &sNum, &sDenom)) return NULL;
	
	DasUnits num;
	if(!_pyd2_units(sNum, &num)) return NULL;
	DasUnits denom;
	if(!_pyd2_units(sDenom, &denom)) return NULL;
	
	bool bRet = DasUnits_canMerge(num, DASOP_DIV, denom);
	if(!bRet){
		PyErr_Format(PyExc_TypeError, "Unsupported operation '/' for units "
				       "%s and %s", sNum, sDenom);
		return NULL;
	}
	
	DasUnits ret = DasUnits_div(num, denom);
	const char* sTo = DasUnits_toStr(ret);
	return Py_BuildValue("s", sTo);
}

static const char pyd2help_unit_pow[] = 
"Raise a set of units to a power\n"
"\n"
"Args:\n"
"  units (str) : The unit set\n"
"  power (int) : The power to raise the units to\n"
"\n"
"Returns:\n"
"  A new units string\n";

static PyObject* pyd2_unit_pow(PyObject* self, PyObject* args)
{
	const char* sUnits = NULL;
	int nPow = 1;
	if(!PyArg_ParseTuple(args, "si:unit_pow", &sUnits, &nPow)) return NULL;
	
	DasUnits units;
	if(!_pyd2_units(sUnits, &units)) return NULL;
	if(units == DASUNITS_DIMENSIONLESS){
		return Py_BuildValue("s", "");
	}

	DasUnits ret = DasUnits_pow(units, nPow);
	const char* sTo = DasUnits_toStr(ret);
	return Py_BuildValue("s", sTo);
}

static const char pyd2help_unit_root[] = 
"Lower the exponents of a set of units to the given root\n"
"\n"
"Args:\n"
"  units (str) : The unit set string\n"
"  root (int)  : The root to lower the units to\n"
"\n"
"Returns:\n"
"  A new units string\n";

static PyObject* pyd2_unit_root(PyObject* self, PyObject* args)
{
	const char* sUnits = NULL;
	int nRoot = 1;
	if(!PyArg_ParseTuple(args, "sd:unit_root", &sUnits, &nRoot)) return NULL;
	
	DasUnits units;
	if(!_pyd2_units(sUnits, &units)) return NULL;
	if(units == DASUNITS_DIMENSIONLESS){
		return Py_BuildValue("s", "");
	}

	DasUnits ret = DasUnits_root(units, nRoot);
	const char* sTo = DasUnits_toStr(ret);
	return Py_BuildValue("s", sTo);
}

static const char pyd2help_unit_invert[] = 
"Invert the exponents of a set of units\n"
"\n"
"Args:\n"
"  units (str) : The unit set\n"
"\n"
"Returns:\n"
"  A new units string\n";

static PyObject* pyd2_unit_invert(PyObject* self, PyObject* args)
{
	const char* sUnits = NULL;
	if(!PyArg_ParseTuple(args, "s:unit_invert", &sUnits)) return NULL;
	
	DasUnits units;
	if(!_pyd2_units(sUnits, &units)) return NULL;
	if(units == DASUNITS_DIMENSIONLESS){
		return Py_BuildValue("s", "");
	}

	DasUnits ret = DasUnits_invert(units);
	const char* sTo = DasUnits_toStr(ret);
	return Py_BuildValue("s", sTo);
}


static const char pyd2help_can_merge [] = 
"See if values in the given units can be merged under a given operation\n"
"\n"
"For interval units (seconds, meters, etc.) multiply and divide always work and\n"
"add and subtract only work when :meth:`_das3.convertible` is True\n."
"Values in reference point units (UTC) can be subtracted to provide an interval\n"
"and intervals can be added or subtracted to references, but references can't\n"
"be added, multiplied or inverted.\n"
"\n"
"Args:\n"
"   left_units (str) : The units of the left side value\n"
"   operator (str) : One of '+', '-', '*', '/', '**', '^' with the traditional\n"
"      meanings\n"
"   right_units (str) : The units of the right side value\n"
"\n"
"Returns: boolean\n"
;

static PyObject* pyd2_can_merge(PyObject* self, PyObject* args)
{
	const char* sLeft = NULL;
	const char* sOp = NULL;
	const char* sRight = NULL;
	
	if(!PyArg_ParseTuple(args, "sss:canMerge", &sLeft, &sOp, &sRight)) return NULL;
	
	DasUnits left, right;
	if(!_pyd2_units(sLeft, &left) || !_pyd2_units(sRight, &right)) return NULL;
	char sError[64] = {'\0'};
	
	int nOp = dasop_binary(sOp);
	if(nOp == DASOP_INVALID){
		snprintf(sError, 63, "Invalid binary operator %s", sOp);
		PyErr_SetString(PyExc_ValueError, sError);
		return NULL;
	}
	bool bRet = DasUnits_canMerge(left, nOp, right);
	if(bRet)  Py_RETURN_TRUE;
	else Py_RETURN_FALSE;
}

/* ************************************************************************* */
/* Include Object Defs */

#include "py_dft.h"
#include "py_builder.h"
#include "py_catalog.h" /* After py_builder.c, uses g_pMgr from py_builder.c */

/*****************************************************************************/
/* The method definitions */

static PyMethodDef pyd2_methods[] = {
	
	/* Stuff from this file */
	{"parsetime",   pyd2_parsetime,   METH_VARARGS, pyd2help_parsetime   },
	{"parse_epoch", pyd2_parse_epoch, METH_VARARGS, pyd2help_parse_epoch },
	{"to_epoch",    pyd2_to_epoch,    METH_VARARGS, pyd2help_to_epoch    },
	{"ttime",       pyd2_ttime,       METH_VARARGS, pyd2help_ttime       },
	{"emitt",       pyd2_emitt,       METH_VARARGS, pyd2help_emitt       },
	{"tnorm",       pyd2_tnorm,       METH_VARARGS, pyd2help_tnorm       },
	{"convertible", pyd2_convertible, METH_VARARGS, pyd2help_convertible },
	{"unit_norm",   pyd2_unit_norm,   METH_VARARGS, pyd2help_unit_norm   },
	{"unit_mul",    pyd2_unit_mul,    METH_VARARGS, pyd2help_unit_mul    },
	{"unit_div",    pyd2_unit_div,    METH_VARARGS, pyd2help_unit_div    },
	{"unit_pow",    pyd2_unit_pow,    METH_VARARGS, pyd2help_unit_pow    },
	{"unit_root",   pyd2_unit_root,   METH_VARARGS, pyd2help_unit_root   },
	{"unit_invert", pyd2_unit_invert, METH_VARARGS, pyd2help_unit_invert },
	{"convert",     pyd2_convert,     METH_VARARGS, pyd2help_convert     },
	{"can_merge",   pyd2_can_merge,   METH_VARARGS, pyd2help_can_merge   },
	{"tt2k_utc",    pyd2_tt2k_utc,    METH_VARARGS, pyd2help_tt2k_utc    },
	{"utc_tt2k",    pyd2_utc_tt2k,    METH_VARARGS, pyd2help_utc_tt2k    },
	
	/* Stuff from py_builder.c */
	{"read_file",   pyd2_read_file,   METH_VARARGS, pyd2help_read_file   },
	{"read_server", pyd2_read_server, METH_VARARGS, pyd2help_read_server },
	{"read_cmd",    pyd2_read_cmd,    METH_VARARGS, pyd2help_read_cmd    }, 
	{"auth_set",    pyd2_auth_set,    METH_VARARGS, pyd2help_auth_set    },
	
	/* Stuff from py_catalog.c */
	{"get_node",    pyd2_get_node,    METH_VARARGS, pyd2help_get_node    },
	{"src_info",    pyd2_src_info,    METH_VARARGS, pyd2help_src_info    },
	{"src_url",     (PyCFunction)(void(*)(void))pyd2_src_url, METH_VARARGS|METH_KEYWORDS, pyd2help_src_url},
	{"src_loadable",pyd2_src_loadable,METH_VARARGS, pyd2help_src_loadable},
	{"mount",       pyd2_mount,       METH_VARARGS, pyd2help_mount       },
	{"unmount",     pyd2_unmount,     METH_VARARGS, pyd2help_unmount     },
	{"mounts",      pyd2_mounts,      METH_VARARGS, pyd2help_mounts      },
	{"seed",        pyd2_seed,        METH_VARARGS, pyd2help_seed        },
	{"seed_info",   pyd2_seed_info,   METH_VARARGS, pyd2help_seed_info   },
	{"check",       pyd2_check,       METH_VARARGS, pyd2help_check       },
	{NULL, NULL, 0, NULL}
};


/*****************************************************************************/
/* Module initialization */

#if PY_MAJOR_VERSION >= 3
/* Thanks to http://python3porting.com/cextensions.html for the porting
 * advice used here */

/* Python 3 initialization structure and function */

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"_das3",                   /* m_name */
	"bindings for libdas2",    /* m_doc */
	-1,                        /* m_size */
	pyd2_methods,              /* m_methods */
	NULL,                      /* m_reload */
	NULL,                      /* m_traverse */
	NULL,                      /* m_clear */
	NULL,                      /* m_free */
};

/* Python 3 signals a failed init by returning NULL with an exception set */
#define INIT_FAIL do{ \
	PyErr_SetString(PyExc_ImportError, "das2C library initialization failed"); \
	return NULL; }while(0)

PyMODINIT_FUNC PyInit__das3(void){
	PyObject* pMod;

	/* This statement is required to setup libdas2.  If you leave it out you
	 * will never get any unit values and errors will act funny.  You may even
	 * get segfaults */
	DasLogHook hook = {pyd2_onLog, NULL};
	if(daslib_init("module _das3", DASERRDISP_RET, DASLOG_INFO, &hook) != DAS_OKAY)
		INIT_FAIL;

	/* Initialize our single credentials manager */
	g_pMgr = new_DasCredMgr();  /* no key file store, as in v2 */
	if(g_pMgr == NULL) INIT_FAIL;
	
	if (PyType_Ready(&pyd2_DftType) < 0)
		return NULL;
	if (PyType_Ready(&pyd2_PsdType) < 0)
		return NULL;
	
	g_pPyD2Error = PyErr_NewException("_das3.Error", PyExc_Exception, NULL);
	Py_INCREF(g_pPyD2Error);
	
	g_pPyD2QueryErr = PyErr_NewException("_das3.BadQuery", PyExc_Exception, NULL);
	Py_INCREF(g_pPyD2QueryErr);
	
	g_pPyD2AuthErr = PyErr_NewException("_das3.Authentication", PyExc_Exception, NULL);
	Py_INCREF(g_pPyD2AuthErr);
	
	if( (pMod = PyModule_Create(&moduledef)) == NULL) return NULL;
	
	PyModule_AddObject(pMod, "Error", g_pPyD2Error);
	PyModule_AddObject(pMod, "QueryError", g_pPyD2QueryErr);
	PyModule_AddObject(pMod, "AuthError", g_pPyD2AuthErr);


	/* This statement is required to setup the numpy C API
 	 * If you leave it out you WILL get SEGFAULTS
 	 */
	import_array();
	
	/* Register the dft objects */
	dft_register(pMod);
	return pMod;
}

#else


/* Python 2 init returns void, failure is just an exception left set */
#define INIT_FAIL do{ \
	PyErr_SetString(PyExc_ImportError, "das2C library initialization failed"); \
	return; }while(0)

PyMODINIT_FUNC init_das3(void){

	/* This statement is required to setup libdas2.  If you leave it out you
	 * will never get any unit values and errors will act funny.  You may even
	 * get segfaults */
	DasLogHook hook = {pyd2_onLog, NULL};
	if(daslib_init("module _das3", DASERRDISP_RET, DASLOG_INFO, &hook) != DAS_OKAY)
		INIT_FAIL;
	
	/* Should probably set an log handler above that ties into python's logging
	 * system */
	
	/* Initialize our single credentials manager */
	g_pMgr = new_DasCredMgr();  /* no key file store, as in v2 */
	if(g_pMgr == NULL) INIT_FAIL;
	
	if (PyType_Ready(&pyd2_DftType) < 0)
		return;
	if (PyType_Ready(&pyd2_PsdType) < 0)
		return;

	g_pPyD2Error = PyErr_NewException("_das3.Error", PyExc_StandardError, NULL);
	Py_INCREF(g_pPyD2Error);
	
	g_pPyD2QueryErr = PyErr_NewException("_das3.BadQuery", PyExc_StandardError, NULL);
	Py_INCREF(g_pPyD2QueryErr);
	
	g_pPyD2AuthErr = PyErr_NewException("_das3.Authentication", PyExc_StandardError, NULL);
	Py_INCREF(g_pPyD2AuthErr);
	
	PyObject* pMod = NULL;
	pMod = Py_InitModule3("_das3", pyd2_methods, "libdas2 python wrappers");
	PyModule_AddObject(pMod, "Error", g_pPyD2Error);
	PyModule_AddObject(pMod, "QueryError", g_pPyD2QueryErr);
	PyModule_AddObject(pMod, "AuthError", g_pPyD2AuthErr);

	/* This statement is required to setup the numpy C API
 	 * If you leave it out you WILL get SEGFAULTS
 	 */
	import_array();
	
	dft_register(pMod);
	/* cords_register(pMod); */
}

#endif
