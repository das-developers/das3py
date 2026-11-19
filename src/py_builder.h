/* Copyright (C) 2017-2026 Chris Piker
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


#include <limits.h>
#include <Python.h>

#include <das3/io.h>
#include <das3/http.h>
#include <das3/units.h>
#include <das3/log.h>
#include <das3/var.h>
#include <das3/form.h>

#include <numpy/ndarrayobject.h>

/*
#include <numpy/npy2_compat.h>
*/

#if PY_MAJOR_VERSION >= 3
#define PyString_FromString PyUnicode_FromString
#define PyInt_FromLong      PyLong_FromLong
#endif

#ifdef _WIN32
#define PyLong_FromInt64 PyLong_FromLongLong
#else
#define PyLong_FromInt64 PyLong_FromLong
#endif

/* This block is just here to make IDE code assistance not throw up lots of
 * errors since it has to be able to understand how to parse this file on
 * it's own. */
#ifndef HAS_DAS2_PYEXCEPT
static PyObject* g_pPyD2Error;
static void* pyd2_setException(void);
static PyObject* g_pPyD2QueryErr;
static PyObject* g_pPyD2AuthErr;
#endif

/* ************************************************************************* */
/* Singleton Credentials manager */

static DasCredMgr* g_pMgr = NULL;

const char pyd2help_auth_set[] =
"Set an authentication hash to be sent to remote servers when certain conditions\n"
"are met.\n"
"\n"
"The request must come from a particular server, for a particular realm,\n"
"and for a particular dataset.  The authentication hash is not transmitted\n"
"unless the server asks for it and the request matches the given conditions.\n"
"\n"
"Args:\n"
"   base_url (str) : The full URL path to the das2 server, which is not\n"
"      typically the same as the host root, example:\n"
"      https://zeus.physics.uiowa.edu/das/server \n"
"\n"
"   realm (str) : The authentication realm.  This is provided in the dsdf\n"
"      files under the securityRealm keyword.\n"
"\n"
"   hash (str) : The hash to send.  Most servers, including das2 pyServer\n"
"      are looking for an HTTP Basic Authentication hash\n"
"\n"
"   dataset (str,optional) : The dataset, ex: 'Juno/WAV/Survey'  Use None\n"
"      to match any dataset in this Realm.  Some sites will\n"
"      not provide this information in which case None should\n"
"      be used to match request from those sites.\n"
"\n"
"HTTP Basic Auth Hash Generation\n"
"   Make the string 'USERNAME:PASSWORD' where ':' is a literal colon and encode\n"
"   it using the base64 algorithm.  The standard_b64encode() function from the\n"
"   python base64 module can be used to perform this task.\n";

static PyObject* pyd2_auth_set(PyObject* self, PyObject* args)
{
	const char* url;  /* full path to CGI */
	const char* realm;
	const char* hash;
	const char* dataset = NULL;

	if(!PyArg_ParseTuple(args, "sss|s:set_auth", &url, &realm, &hash, &dataset))
		return NULL;

	/* A record and its secret are set separately now.  The hash is already
	 * the base64 "user:password" that DASAUTH_BASIC sends as is. */
	int16_t nCred = DasCredMgr_addCred(g_pMgr, url, realm, dataset);
	if(nCred < 0){
		PyErr_SetString(PyExc_ValueError,
			"Credential not stored, a string was too long or the manager is full");
		return NULL;
	}
	if(DasCredMgr_setSecret(g_pMgr, nCred, DASAUTH_BASIC, hash, NULL) != DAS_OKAY){
		PyErr_SetString(PyExc_ValueError, "Authentication hash is too long");
		return NULL;
	}

	/* Same return as v2: the number of credentials held */
	return Py_BuildValue("i", DasCredMgr_numCreds(g_pMgr));
}

/* Helper for NDarray creation */
static void _npdims_from_shape(DasAry* pAry, PyArray_Dims* pNpDims){
	ptrdiff_t shape[16] = {0};
	DasAry_shape(pAry, shape);

	int nRank = DasAry_rank(pAry);
	pNpDims->len = nRank;
	for(int i = 0; i < nRank; ++i) pNpDims->ptr[i] = shape[i];
}

/* ************************************************************************ */
/* Make a new numpy datetime64 array, allocs ndarray memory                 */

PyObject* _DasCalAryToNumpyAry(DasAry* pAry)
{
	/* Access the das2 array and the numpy array as a flat index space makes
	 * looping faster as the for-loop to find the next multi-dimensional
	 * index is not required */
	ptrdiff_t nLen = 0;
	das_vt vt = DasAry_vt(pAry);
	const void* pMem = DasAry_valsIn(pAry, vt, DAS_ADDR0, &nLen);
	if(nLen < 0) return pyd2_setException(g_pPyD2Error);
	size_t uLen = (size_t)nLen;

	/* Create a Numpy Array descriptor that says we're going to use nanoseconds
	 * since 1970-01-01 for the meaning of the stored int8_t values */
	PyArray_Descr* pDesc = NULL;
	PyObject* pType = PyString_FromString("M8[ns]");
	PyArray_DescrConverter(pType, &pDesc);
	Py_DECREF(pType);

	npy_intp npLen = (npy_intp)uLen;
	PyObject* pObj = PyArray_SimpleNewFromDescr(1, &npLen, pDesc);
	if(pObj == NULL){
		PyErr_Format(g_pPyD2Error, "Couldn't generate new datetime64 array ");
		return NULL;
	}
	if(!PyArray_Check(pObj)){
		PyErr_SetString(PyExc_TypeError, "Unexpected type in _DasCalAryToNumpyAry()");
		Py_DECREF(pObj);
		return NULL;
	}

	PyArrayObject* pNdAry = (PyArrayObject*)pObj;

	if(! PyArray_ISBEHAVED(pNdAry) || !(PyArray_IS_C_CONTIGUOUS(pNdAry))){
		PyErr_Format(g_pPyD2Error, "New NDArray is not contiguous, aligned, and "
				                     "in machine byte order");
		Py_DECREF(pNdAry);
		return NULL;
	}

	/* Type branch to keep inner loops small */
	const uint8_t* pUByte;   const int8_t* pByte;
	const int16_t* pShort; const uint16_t* pUShort;
	const int32_t* pInt;   const uint32_t* pUInt;
	const int64_t* pLong;  const uint64_t* pULong;
	const float* pFloat;   const double* pDbl;
	const DasTime* pDt;
	npy_intp nd_index0[16] = {'\0'};
	int64_t* pNdData = (int64_t*) PyArray_GetPtr((PyArrayObject*)pNdAry, nd_index0);

	size_t u = 0;
	DasTime dt;
	DasUnits units = DasAry_units(pAry);
	switch(vt){
	case DASVT_UBYTE:
		for(pUByte = (const uint8_t*)pMem, u = 0; u < uLen; ++u, ++pUByte, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pUByte), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_BYTE:
		for(pByte = (const int8_t*)pMem, u = 0; u < uLen; ++u, ++pByte, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pByte), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_USHORT:
		for(pUShort = (const uint16_t*)pMem, u = 0; u < uLen; ++u, ++pUShort, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pUShort), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_SHORT:
		for(pShort = (const int16_t*)pMem, u = 0; u < uLen; ++u, ++pShort, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pShort), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_UINT:
		for(pUInt = (const uint32_t*)pMem, u = 0; u < uLen; ++u, ++pUInt, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pUInt), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_INT:
		for(pInt = (const int32_t*)pMem, u = 0; u < uLen; ++u, ++pInt, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pInt), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_ULONG:
		for(pULong = (const uint64_t*)pMem, u = 0; u < uLen; ++u, ++pULong, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pULong), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_LONG:
		for(pLong = (const int64_t*)pMem, u = 0; u < uLen; ++u, ++pLong, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pLong), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_FLOAT:
		for(pFloat = (const float*)pMem, u = 0; u < uLen; ++u, ++pFloat, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pFloat), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_DOUBLE:
		for(pDbl = (const double*)pMem, u = 0; u < uLen; ++u, ++pDbl, ++pNdData) {
			DasTime_fromPoint(&dt, (double)(*pDbl), units);
			*pNdData = DasTime_toNs1970(&dt);
		}
		break;
	case DASVT_TIME:
		for(pDt = (const DasTime*)pMem, u = 0; u < uLen; ++u, ++pDt, ++pNdData){
			*pNdData = DasTime_toNs1970((DasTime*)pDt);
		}
		break;
	default:
		PyErr_Format(g_pPyD2Error, "Type vt = %d not supported in conversion "
				       "to datetime64", vt);
		Py_DECREF(pNdAry);
		return NULL;
	}

	/* Reshape the array (hopefully IN PLACE, don't know how to insure this) */
	npy_intp np_shape[16] = {0};
	PyArray_Dims np_dims = {np_shape, 0};
	_npdims_from_shape(pAry, &np_dims);
	PyObject* pFinal = PyArray_Newshape((PyArrayObject*)pNdAry, &np_dims, NPY_CORDER);
	Py_DECREF(pNdAry);

	return pFinal;
}

/* ************************************************************************ */
/* Make a new numpy timedelta64 array, allocs ndarray memory                */

PyObject* _DasTimeAryToNumpyAry(DasAry* pAry)
{
	/* Get the conversion factor to nanoseconds */
	DasUnits units = DasAry_units(pAry);
	double factor = DasUnits_convert(1.0, units, DASUNITS_SECONDS);
	if(factor == DASVAL_FILL) return NULL;
	factor *= 1.0e+9;

	ptrdiff_t nLen = 0;
	das_vt vt = DasAry_vt(pAry);
	const void* pMem = DasAry_valsIn(pAry, vt, DAS_ADDR0, &nLen);
	if(nLen < 0) return pyd2_setException(g_pPyD2Error);
	size_t uLen = (size_t)nLen;

	/* Create a Numpy Array descriptor that says we're going to use nanoseconds
	 * for the meaning of the stored int8_t values */
	PyArray_Descr* pDesc = NULL;
	PyObject* pType = PyString_FromString("m8[ns]");
	PyArray_DescrConverter(pType, &pDesc);
	Py_DECREF(pType);

	npy_intp npLen = (npy_intp)uLen;
	PyObject* pObj = PyArray_SimpleNewFromDescr(1, &npLen, pDesc);
	if(pObj == NULL){
		PyErr_Format(g_pPyD2Error, "Couldn't generate new timedelta64 array ");
		return NULL;
	}
	PyArrayObject* pNdAry = (PyArrayObject*)pObj;

	if(! PyArray_ISBEHAVED(pNdAry) || !(PyArray_IS_C_CONTIGUOUS(pNdAry))){
		PyErr_Format(g_pPyD2Error, "New NDArray is not contiguous, aligned, and "
				                     "in machine byte order");
		Py_DECREF(pNdAry);
		return NULL;
	}

	/* Type branch to keep inner loops small */
	const uint8_t* pUByte; const int8_t* pByte; 
	const int16_t* pShort; const uint16_t* pUShort;
	const uint32_t* pUInt; const int32_t* pInt; 
	const uint64_t* pULong; const int64_t* pLong;  
	const float* pFloat; const double* pDbl;
	double dTmp = 0.0;
	float  fTmp = 0.0f;
	npy_intp nd_index0[16] = {'\0'};
	int64_t* pNdData = (int64_t*) PyArray_GetPtr((PyArrayObject*)pNdAry, nd_index0);

	size_t u = 0;
	switch(vt){
	case DASVT_UBYTE:
		for(pUByte = (const uint8_t*)pMem, u = 0; u < uLen; ++u, ++pUByte, ++pNdData) {
			*pNdData = (long int)((*pUByte)*factor);
		}
		break;
	case DASVT_BYTE:
		for(pByte = (const int8_t*)pMem, u = 0; u < uLen; ++u, ++pByte, ++pNdData) {
			*pNdData = (long int)((*pByte)*factor);
		}
		break;
	case DASVT_SHORT:
		for(pShort = (const int16_t*)pMem, u = 0; u < uLen; ++u, ++pShort, ++pNdData) {
			*pNdData = (long int)((*pShort)*factor);
		}
		break;
	case DASVT_USHORT:
		for(pUShort = (const uint16_t*)pMem, u = 0; u < uLen; ++u, ++pUShort, ++pNdData) {
			*pNdData = (long int)((*pUShort)*factor);
		}
		break;
	case DASVT_UINT:
		for(pUInt = (const uint32_t*)pMem, u = 0; u < uLen; ++u, ++pUInt, ++pNdData) {
			*pNdData = (long int)((*pUInt)*factor);
		}
		break;
	case DASVT_INT:
		for(pInt = (const int32_t*)pMem, u = 0; u < uLen; ++u, ++pInt, ++pNdData) {
			*pNdData = (long int)((*pInt)*factor);
		}
		break;
	case DASVT_ULONG:
		for(pULong = (const uint64_t*)pMem, u = 0; u < uLen; ++u, ++pULong, ++pNdData) {
			*pNdData = (long int)((*pULong)*factor);
		}
		break;
	case DASVT_LONG:
		for(pLong = (const int64_t*)pMem, u = 0; u < uLen; ++u, ++pLong, ++pNdData) {
			*pNdData = (long int)((*pLong)*factor);
		}
		break;
	case DASVT_FLOAT:
		/* Problem for time deltas!  The common fill value -1e31 is less than
		 * minimum possible long int!  We need to convert these fill values to
		 * something that is possible. */
		for(pFloat = (const float*)pMem, u = 0; u < uLen; ++u, ++pFloat, ++pNdData) {
			fTmp = (float) ((*pFloat) * factor);
			if((fTmp < LONG_MIN)||(fTmp > LONG_MAX))
				*pNdData = DASVAL_FILL_INT64;
			else
				*pNdData = (long int)(fTmp);
		}
		break;
	case DASVT_DOUBLE:
		/* See fill value problem above */
		for(pDbl = (const double*)pMem, u = 0; u < uLen; ++u, ++pDbl, ++pNdData) {
			dTmp = (*pDbl)*factor;
			if((dTmp < LONG_MIN)||(dTmp > LONG_MAX))
				*pNdData = DASVAL_FILL_INT64;
			else
				*pNdData = (long int)(dTmp);
		}
		break;
	default:
		PyErr_Format(g_pPyD2Error, "Type vt = %d not supported in conversion "
				       "to timedelta64", vt);
		Py_DECREF(pNdAry);
		return NULL;
	}

	/* Reshape the array (hopefully IN PLACE, don't know how to insure this) */
	npy_intp np_shape[16] = {0};
	PyArray_Dims np_dims = {np_shape, 0};
	_npdims_from_shape(pAry, &np_dims);
	PyObject* pFinal = PyArray_Newshape((PyArrayObject*)pNdAry, &np_dims, NPY_CORDER);
	Py_DECREF(pNdAry);

	return pFinal;
}

/* ************************************************************************ */
/* Allocs each string's memory and the ndarray memory (slow)                */

PyObject* _DasTextAryToNumpyAry(DasAry* pAry)
{
	das_vt vt = DasAry_vt(pAry);

	/* Das2 DASVT_TEXT type is used for arrays of constant pointers to null
	 * terminated strings.  Since there's so many things that can go wrong
	 * with not owning your own memory, I don't use it much, but it does
	 * exist.  DASVT_BYTE is much more common */

	if((vt != DASVT_TEXT)&&(vt != DASVT_BYTE)){
		PyErr_Format(g_pPyD2Error, "Logic error in %s,%d", __FILE__, __LINE__);
		return NULL;
	}

	ptrdiff_t shape[16] = {0};
	int nRank = DasAry_shape(pAry, shape);
	int nAsRank = nRank;

	if(vt == DASVT_BYTE) nAsRank = nRank - 1; /* For these arrays, last index is
													   * just the character count */

	/* Make sure we aren't ragged (except for the last dimension of DASVT_BYTE
	 * arrays of coarse. */
	for(int d = 0; d < nAsRank; ++d){
		if(shape[d] == DASIDX_RAGGED){
			PyErr_Format(g_pPyD2Error, "Ragged array translation is not yet implemented");
			return NULL;
		}
	}

	int i;
	npy_intp np_shape[16] = {0};
	for(i = 0; i < nAsRank; ++i) np_shape[i] = shape[i];

	/* Create the array allocating space for the pointers */
	PyObject* pObj = PyArray_SimpleNew(nAsRank, np_shape, NPY_OBJECT);
	PyArrayObject* pNdAry = (PyArrayObject*)pObj;

	if(! PyArray_ISCONTIGUOUS(pNdAry) || ! PyArray_ISBEHAVED(pNdAry)){
		PyErr_Format(g_pPyD2Error, "New NDArray is not behaved");
		Py_DECREF(pNdAry);
		return NULL;
	}
	if(! PyDataType_REFCHK( PyArray_DESCR(pNdAry) ) ){
		PyErr_Format(g_pPyD2Error, "New NDArray is not handling reference "
				       "counting for us");
		Py_DECREF(pNdAry);
		return NULL;
	}

	/* We have to do formal multidimensional iteration instead of just getting
	 * the memory pointer and doing a flat iteration because the strings in
	 * DASVT_BYTE could contain many nulls in a row. There's no guaruntee that each
	 * string has only a single null at the end. */
	npy_intp np_index[16] = {0};
	ptrdiff_t d2_index[16] = {0};

	const void* pItem = NULL;
	const char* sStr = NULL;
	PyObject** ppSet = NULL;
	PyObject* pStr = NULL;
	ptrdiff_t nStrLen = 0;

	bool bNext = (DasAry_size(pAry) > 0);
	while(bNext){
		pItem = DasAry_valsIn(pAry, vt, nAsRank, (ptrdiff_t*)d2_index, &nStrLen);
		if(nStrLen < 0){
			Py_DECREF(pNdAry);  /* frees the strings already set in it */
			return pyd2_setException(g_pPyD2Error);
		}

		if(pItem == NULL) sStr = NULL;  /* an empty ragged row */
		else if(vt == DASVT_TEXT) sStr = *( (const char**) pItem);
		else sStr = (const char*) pItem;

		ppSet = (PyObject**) PyArray_GetPtr(pNdAry, np_index);

		if((sStr != NULL)&&(sStr[0] != '\0')){
			pStr = PyString_FromString(sStr);
		}
		else{
			pStr = Py_None;
			Py_INCREF(Py_None);
		}
		*ppSet = pStr;

		/* Don't dec the reference here, the numpy object owns it */

		/* Next index please... */
		bNext = false;
		for(i = nAsRank - 1; i >= 0; --i){
			if(d2_index[i] < (shape[i] - 1)){
				d2_index[i] += 1;
				np_index[i] += 1;
				bNext = true;
				break;
			}
			else{
				d2_index[i] = 0;
				np_index[i] = 0;
			}
		}
	}
	return (PyObject*)pNdAry; /* Cast it back */
}

/* ************************************************************************ */
/* Convert a DasAry to NDarray with coping data (fast)                      */

static PyObject* _DasGenericAryToNumpyAry(DasAry* pAry)
{
	char sInfo[64] = {'\0'};        /* For error messages */
	DasAry_toStr(pAry, sInfo, 63);

	npy_intp pa_shape[16] = {0};
	PyArray_Dims np_dims = {pa_shape, 0};
	_npdims_from_shape(pAry, &np_dims);

	/* Assume check in higher level function for raggedness */
	int nType = 0;
	das_vt et = DasAry_vt(pAry);
	switch(et){
	case DASVT_UBYTE:  nType = NPY_UINT8; break;
	case DASVT_BYTE:   nType = NPY_INT8; break;
	case DASVT_USHORT: nType = NPY_UINT16; break;
	case DASVT_SHORT:  nType = NPY_INT16; break;
	case DASVT_UINT:   nType = NPY_UINT32; break;
	case DASVT_INT:    nType = NPY_INT32; break;
	case DASVT_ULONG:  nType = NPY_UINT64; break;
	case DASVT_LONG:   nType = NPY_INT64; break;
	case DASVT_FLOAT:  nType = NPY_FLOAT32; break;
	case DASVT_DOUBLE: nType = NPY_FLOAT64; break;
	default:
		/* Can't handle unknown types for now, could return these as just byte */
		/* blobs to python in the future, might be handy for telemetry */
		PyErr_Format(g_pPyD2Error, "Logic error in %s,%d", __FILE__, __LINE__);
		return NULL;
	}

	/* Make sure das2 arrays don't delete their data when free'ed */
	size_t uLen = 0;
	size_t uOffset = 0;
	uint8_t* pMem = DasAry_disownData(pAry, &uLen, &uOffset);
	PyObject* pNdAry = NULL;
	
	if(uLen != 0){
		if(pMem == NULL){
			PyErr_Format(g_pPyD2Error, "Array %s does not own it's elements",sInfo);
			return NULL;
		}
		if(uOffset > 0){
			PyErr_Format(g_pPyD2Error, "Array %s has head trim, update das3py",sInfo);
			free(pMem);  /* You owned it, so clean it up */
			return NULL;
		}

		pNdAry = PyArray_SimpleNewFromData(np_dims.len, np_dims.ptr, nType, pMem);

		/* Interface is incomplete if we have to do a cast like this just to
		 * give memory away.  Will be REALLY nice to stop supporting Centos 6
		 * and it's ancient libraries. */
		/* ((PyArrayObject*)pNdAry)->flags |= NPY_OWNDATA;  // for numpy < 1.7 */
		PyArray_ENABLEFLAGS((PyArrayObject*)pNdAry, NPY_ARRAY_OWNDATA);
	}
	else{
		pNdAry = PyArray_SimpleNew(np_dims.len, np_dims.ptr, nType);
	}

	return pNdAry;
}

/* ************************************************************************ */
/* Converting any* DasAry to ndarray without a data copy if possible        */

/* Note that DasAry is more flexible in one respect in that all it's
 * dimensions can be ragged.  Since ndarrays allow for masks we can get around
 * numpy's limitation by making a cubic array and giving it a mask.  For now
 * I'm just not supporting ragged arrays (except of DASVT_BYTE arrays that store
 * strings) since das 2.2 streams don't have them anyway.
 *
 * * TODO: Handling mask creation for ragged arrays
 *
 * Basic conversion is handled as follows:
 *
 *  1. If the units of the array are epoch times (no matter the data type),
 *     generate an array of numpy datetime64 objects with units ns (nanoseconds).
 *
 *  2. If the units of the array are convertible to seconds (no mater the
 *     data type), output an array of numpy timedelta64 objects.
 *
 *  3. If the type of the array is DASVT_TEXT (no matter the units), or if the type
 *     is DASVT_BYTE and the flag DASARY_AS_STRING is set, output a python string
 *     object array
 *
 *  4. Otherwise output a generic C-aligned basic type array
 */
static PyObject* _DasAryToNumpyAry(DasAry* pAry)
{
	DasUnits units = DasAry_units(pAry);
	das_vt vt = DasAry_vt(pAry);

	if((vt == DASVT_TIME) || DasUnits_isTimePoint(units))
		return _DasCalAryToNumpyAry(pAry);

	if(DasUnits_canConvert(units, DASUNITS_SECONDS))
		return _DasTimeAryToNumpyAry(pAry);

	unsigned int uFlags = DasAry_usage(pAry);
	if((vt == DASVT_TEXT) || ( (uFlags&DASARY_AS_STRING) == DASARY_AS_STRING) )
		return _DasTextAryToNumpyAry(pAry);

	return _DasGenericAryToNumpyAry(pAry);
}

/* ************************************************************************* */
/* Create python fill values from array fill values                          */
/* Note that the canonical DASVAL_FILL only works for floating point      */
/* types.  If array is to be converted to a timedelta64 or datetime64 we     */
/* need to substitute fill values than are in range                          */

static PyObject* _DasAryFillToObj(DasAry* pAry)
{
	das_vt vt = DasAry_vt(pAry);
	DasUnits    units = DasAry_units(pAry);
	const void* vpFill = DasAry_fill(pAry);

	int64_t nFill = 0;
	double  rFill = 0.0;

	switch(vt){
	case DASVT_UBYTE:  nFill = *((uint8_t*)vpFill);    break;
	case DASVT_BYTE:   nFill = *((int8_t*)vpFill);   break;
	case DASVT_USHORT: nFill = *((uint16_t*)vpFill); break;
	case DASVT_SHORT:  nFill = *((int16_t*)vpFill);  break;
	case DASVT_UINT:   nFill = *((uint32_t*)vpFill); break;
	case DASVT_INT:    nFill = *((int32_t*)vpFill);  break;
	case DASVT_ULONG:  nFill = *((uint64_t*)vpFill); break;
	case DASVT_LONG:   nFill = *((int64_t*)vpFill);  break;
	case DASVT_FLOAT:  rFill = *((float*)vpFill);    break;
	case DASVT_DOUBLE: rFill = *((double*)vpFill);   break;
	case DASVT_TIME:  /* TODO: Handle fill value for DasTime objects */
	case DASVT_TEXT:
		Py_INCREF(Py_None);
		return Py_None;
		break;
	default:
		/* Can't handle unknown types for now.  Would need a size call back */
		PyErr_Format(g_pPyD2Error, "Code logic error will setting fill value for "
				       "array %s", DasAry_id(pAry));
		return NULL;
	}
	
	/* This compliments code in _DasTimeAryToNumpyAry above.
	 * That function sets any values outside of LONG_MIN, LONG_MAX to 
	 * DASVAL_FILL_INT64, which is almost always what you want.
	 * 
	 * The fill value itself is scaled to nanoseconds as needed.
	 */
	double factor = 1.0;
	if(DasUnits_isTimePoint(units) || DasUnits_canConvert(units, DASUNITS_SECONDS)){
		
		/* Get the conversion factor to nanoseconds */
		if(DasUnits_canConvert(units, DASUNITS_SECONDS)){
			factor = DasUnits_convert(1.0, units, DASUNITS_SECONDS);
			if(factor == DASVAL_FILL){
				Py_INCREF(Py_None);
				return Py_None;
			}
			factor *= 1.0e+9;
		}
		/* TODO: Handle FILL for calendar time objects */
		
		if((vt == DASVT_DOUBLE)||(vt == DASVT_FLOAT)){
			if((rFill*factor < LONG_MIN)||(rFill*factor > LONG_MAX))
				return PyLong_FromInt64(DASVAL_FILL_INT64);
			else
				return PyLong_FromInt64((int64_t)(rFill*factor));
		}	
		else{
			if((nFill*factor < LONG_MIN)||(nFill*factor > LONG_MAX)){
				return PyLong_FromInt64(DASVAL_FILL_INT64);
			}
			else
				return PyLong_FromInt64((int64_t)(nFill*factor));
		}
	}
	else{
		if((vt == DASVT_DOUBLE)||(vt == DASVT_FLOAT))
			return PyFloat_FromDouble(rFill);
		else
			return PyLong_FromInt64(nFill);
	}
}

/* ************************************************************************* */
/* Convert DasDesc to Python dictionary of the form  name : (type, value)    */

static PyObject* _props2PyDict(DasDesc* pDesc)
{
	const char* sUnits = NULL;
	PyObject* pDict = PyDict_New();
	PyObject* pTup;

	size_t uProps = DasDesc_numProps(pDesc);
	for(size_t u = 0; u < uProps; ++u){

		const DasProp* pProp = DasDesc_propAt(pDesc, u);
		if(pProp == NULL) continue;  /* Unlikely, but possible */

		DasUnits units = DasProp_units(pProp);
		if((units == NULL)||(units[0] == '\0'))
			sUnits = "";
		else
			sUnits = units;
		/* A one character string, not a pointer at a lone char: PyString reads
		 * to the NUL, and a bare char on the stack has none after it. */
		char sSep[2] = {DasProp_sep(pProp), '\0'};

		/* Set before range: DASPROP_SET is both bits, so a set answers true
		 * to DasProp_isRange() as well. */
		int nMultiplicity = 1;
		if(DasProp_isSet(pProp)) nMultiplicity = 3;
		else if(DasProp_isRange(pProp)) nMultiplicity = 2;

		pTup = Py_BuildValue("ssssi", 
			DasProp_typeStr3(pProp), DasProp_value(pProp), sUnits, sSep,
			nMultiplicity
		);
		
		if(pTup == NULL){
			Py_DECREF(pDict); return NULL;
		}
		if(PyDict_SetItemString(pDict, DasProp_name(pProp), pTup) != 0){
			Py_DECREF(pTup); Py_DECREF(pDict); return NULL;
		}
	}
	return pDict;
}

/* ************************************************************************* */
/* Per-variable formalism: the <ops> element.
 *
 * Frames, bodies and coordinate systems ride on each variable's formalism
 * (DasForm); there is no stream level registry to walk.  A form is optional,
 * strings and blobs have none, and a kind das2C does not recognize still
 * arrives, as the generic form with its parameters carried verbatim. */

/* Hand a new reference to a dict and drop ours.  False if the value was
 * never built or the insert failed, so callers can chain with && and let
 * short-circuit skip the rest. */
static bool _setDictItem(PyObject* pDict, const char* sKey, PyObject* pVal)
{
	if(pVal == NULL) return false;
	int nRet = PyDict_SetItemString(pDict, sKey, pVal);
	Py_DECREF(pVal);
	return (nRet == 0);
}

/* Every parameter name a known kind can answer for.  A kind returns NULL for
 * names it does not define, so one list serves all of them and the dict only
 * carries what the form actually holds.  Order here is dict order. */
static const char* g_sFormParams[] = {
	"frame", "body", "fixed", "system", "sysorder", "surface", "from", "to", NULL
};

/* Booleans are the only parameter das2C hands back decoded (as "true" or
 * "false"), everything else keeps its wire spelling. */
static PyObject* _formParam2Py(const char* sVal, uint8_t uType)
{
	if((uType & DASPROP_TYPE_MASK) == DASPROP_BOOL)
		return PyBool_FromLong(strcmp(sVal, "true") == 0);
	return PyString_FromString(sVal);
}

/* dasvt_name() answers NULL for a type it has no arm for, which as of
 * das2C 65fb971 includes DASVT_COMP.  Never hand that to PyString. */
static const char* _valTypeStr(das_vt vt)
{
	const char* sType = dasvt_name(vt);
	if(sType != NULL) return sType;
	return (vt == DASVT_COMP) ? "composite" : "unknown";
}

/* The backing array's id, or None for a computed variable (a sequence, a
 * constant, or reference + offset).  The python side binds ndarrays by this
 * key; the expression string is for people. */
static PyObject* _varArrayId2Py(const DasVar* pVar)
{
	const DasAry* pAry = DasVar_ary(pVar);
	if(pAry == NULL) Py_RETURN_NONE;
	return PyString_FromString(DasAry_id(pAry));
}

/* External index -> array index, one entry per dataset index, None where
 * the variable is degenerate in that index.  Only an array generator has a
 * map; everything else answers None so callers can test one key. */
static PyObject* _varIdxMap2PyList(const DasVar* pVar, int nDsRank)
{
	int8_t aMap[DASIDX_MAX];
	int nExtRank = DasVar_idxMap(pVar, aMap);
	if(nExtRank < 0) Py_RETURN_NONE;

	PyObject* pList = PyList_New(nDsRank);
	if(pList == NULL) return NULL;

	for(int i = 0; i < nDsRank; ++i){
		PyObject* pItem = NULL;
		if((i >= nExtRank)||(aMap[i] == DASIDX_UNUSED)){
			Py_INCREF(Py_None); pItem = Py_None;
		}
		else{
			pItem = PyLong_FromLong(aMap[i]);
		}
		if(pItem == NULL){ Py_DECREF(pList); return NULL; }
		PyList_SET_ITEM(pList, i, pItem);  /* steals the reference */
	}
	return pList;
}

/* Sequences and constants have no backing array, so the python side would
 * have nothing to bind and the coordinate would silently vanish.  Compute the
 * values once here over the dataset's extent, with every degenerate index
 * pinned to one element and then dropped, so the result is as compact as a
 * stored array would be.  It joins 'arrays' under the name dim.role, which
 * cannot collide with a wire id.
 *
 * Reference + offset (a DasVarBin) is deliberately NOT done here.  The python
 * Dimension makes that center itself, and doubling a waveform's memory to
 * save it the trouble is a poor trade. */
static bool _materialize(
	const DasDs* pDs, const DasDim* pDim, const char* sRole, const DasVar* pVar,
	PyObject* pVarDict, PyObject* pdArys, PyObject* pdFill
){
	ptrdiff_t aShape[DASIDX_MAX] = {0};
	ptrdiff_t aMin[DASIDX_MAX] = {0};
	ptrdiff_t aMax[DASIDX_MAX] = {0};
	int nRank = DasDs_shape(pDs, aShape);

	PyObject* pIdxMap = PyList_New(nRank);
	if(pIdxMap == NULL) return false;

	int nMapped = 0;
	for(int i = 0; i < nRank; ++i){
		PyObject* pItem = NULL;
		if(DasVar_isDegenerate(pVar, i)){
			aMax[i] = 1;
			Py_INCREF(Py_None); pItem = Py_None;
		}
		else{
			aMax[i] = aShape[i];
			pItem = PyLong_FromLong(nMapped++);
		}
		if(pItem == NULL){ Py_DECREF(pIdxMap); return false; }
		PyList_SET_ITEM(pIdxMap, i, pItem);  /* steals the reference */
	}

	/* materialize, not subset: the numeric converter below hands the
	 * element memory to numpy and needs the array to own it outright */
	DasAry* pAry = new_DasAry_materialize(pVar, nRank, aMin, aMax, NULL);
	if(pAry == NULL){
		Py_DECREF(pIdxMap);
		pyd2_setException(g_pPyD2Error);
		return false;
	}

	PyObject* pFill = _DasAryFillToObj(pAry);
	PyObject* pNd = _DasAryToNumpyAry(pAry);
	dec_DasAry(pAry);
	if((pFill == NULL)||(pNd == NULL)){
		Py_XDECREF(pFill); Py_XDECREF(pNd); Py_DECREF(pIdxMap);
		return false;
	}

	/* Drop the pinned axes, but only if they are there to drop.  As of das2C
	 * 65fb971 an array backed variable answers dataset shaped (rank 3 in,
	 * rank 3 plus internal out) while a sequence answers already compact,
	 * so decide from the ndarray's own dims rather than assume either.  The
	 * ndarray is used rather than the DasAry since the text converter folds
	 * the character axis away. */
	int nNd = PyArray_NDIM((PyArrayObject*)pNd);
	const npy_intp* pNdDims = PyArray_DIMS((PyArrayObject*)pNd);
	bool bDsShaped = (nNd >= nRank);
	for(int i = 0; bDsShaped && (i < nRank); ++i)
		bDsShaped = (pNdDims[i] == (npy_intp)aMax[i]);

	PyObject* pCompact = pNd;
	if(bDsShaped && (nMapped < nRank)){
		npy_intp aDims[2*DASIDX_MAX] = {0};
		PyArray_Dims newDims = {aDims, 0};
		for(int i = 0; i < nNd; ++i){
			if((i < nRank) && DasVar_isDegenerate(pVar, i)) continue;
			aDims[newDims.len++] = pNdDims[i];
		}
		if(newDims.len == 0){ aDims[0] = 1; newDims.len = 1; }  /* a lone constant */

		pCompact = PyArray_Newshape((PyArrayObject*)pNd, &newDims, NPY_CORDER);
		Py_DECREF(pNd);
		if(pCompact == NULL){ Py_DECREF(pFill); Py_DECREF(pIdxMap); return false; }
	}

	char sAryId[256] = {'\0'};
	snprintf(sAryId, 255, "%s.%s", DasDim_name(pDim), sRole);

	bool bOkay =
		_setDictItem(pdArys,   sAryId,   pCompact) &&
		_setDictItem(pdFill,   sAryId,   pFill) &&
		_setDictItem(pVarDict, "array",  PyString_FromString(sAryId)) &&
		_setDictItem(pVarDict, "idxmap", pIdxMap);
	return bOkay;
}

/* ************************************************************************* */
/* Summaries of what das2py hands back, not of what das2C read               */
/*                                                                           */
/* DasVar_toStr() and DasDs_info() describe das2C's variables.  By the time  */
/* a dataset reaches python das2py has changed some of them:                 */
/*                                                                           */
/*   - Time points are datetime64[ns] and durations timedelta64[ns], so      */
/*     int64 in ns1970 or ns, see _DasAryToNumpyAry().                       */
/*   - Sequences and constants are arrays named <dim>.<role>, see            */
/*     _materialize().                                                       */
/*                                                                           */
/* The summaries are rewritten to match, and the das2C objects are left      */
/* alone.  The v3.0 summary formats are a stable interface, which is what    */
/* makes editing them sound; text that doesn't parse as v3.0 is an error     */
/* rather than being passed through mislabeled.                              */

#if PY_MAJOR_VERSION >= 3
#define _pyd2_utf8(o) PyUnicode_AsUTF8(o)
#else
#define _pyd2_utf8(o) PyString_AsString(o)
#endif

/* The units a variable's values have once in numpy, or NULL when das2py
 * leaves them as read.  The tests mirror _DasAryToNumpyAry(). */
static DasUnits _pyd2_npUnits(const DasVar* pVar)
{
	DasUnits units = DasVar_units(pVar);
	if((DasVar_vt(pVar) == DASVT_TIME) || DasUnits_isTimePoint(units))
		return DasUnits_fromStr("ns1970");
	if(DasUnits_canConvert(units, DASUNITS_SECONDS))
		return DasUnits_fromStr("ns");
	return NULL;
}

/* The element name as DasVar_toStr() prints it */
static const char* _pyd2_elemName(const DasVar* pVar)
{
	das_et et = DasVar_et(pVar);
	const char* sElem = (et == DASET_TIME) ? "time" : dasvt_name((das_vt)et);
	return (sElem != NULL) ? sElem : "unknown";
}

/* The v3.0 one line summary, "<generator> <element> [<units>] | <shape> |
 * <formalism>", with the element and units changed to what numpy holds.  A
 * materialized variable gets its synthesized array as the generator, with
 * one index letter per entry in the shape section, which lists the same
 * axes in the same order as the compacted ndarray. */
static PyObject* _pyd2_varExpr(
	const DasDim* pDim, const char* sRole, const DasVar* pVar, bool bMaterial
){
	char sOrig[DASVAR_STR_SZ] = {'\0'};  /* always holds the whole line */
	DasVar_toStr(pVar, sOrig, sizeof(sOrig));

	const char* pSec2 = strstr(sOrig, " | ");
	DasUnits units = DasVar_units(pVar);
	const char* sUnits = (units != NULL) ? units : "";
	const char* sElem  = _pyd2_elemName(pVar);

	char sTail[256] = {'\0'};
	if(sUnits[0] != '\0') snprintf(sTail, sizeof(sTail), " %s %s", sElem, sUnits);
	else                  snprintf(sTail, sizeof(sTail), " %s", sElem);

	size_t uSec1 = (pSec2 != NULL) ? (size_t)(pSec2 - sOrig) : 0;
	size_t uTail = strlen(sTail);
	if((pSec2 == NULL) || (uTail > uSec1) ||
	   (strncmp(sOrig + uSec1 - uTail, sTail, uTail) != 0))
		return PyErr_Format(g_pPyD2Error,
			"Summary of variable %s:%s is not in the das v3.0 format: '%s'",
			DasDim_name(pDim), sRole, sOrig
		);
	pSec2 += 3;

	DasUnits npUnits = _pyd2_npUnits(pVar);
	if(npUnits != NULL){
		sElem  = dasvt_name(DASVT_LONG);  /* both numpy time types are int64 */
		sUnits = npUnits;
	}

	char sGen[512] = {'\0'};
	if(bMaterial){
		int n = snprintf(sGen, sizeof(sGen), "%s.%s", DasDim_name(pDim), sRole);
		const char* pEnd = strstr(pSec2, " | ");
		if(pEnd == NULL) pEnd = pSec2 + strlen(pSec2);
		for(const char* p = pSec2; (p < pEnd) && (n < (int)sizeof(sGen) - 4); ++p){
			if((p[1] == ':') && ((p == pSec2) || (p[-1] == ' ')))
				n += snprintf(sGen + n, sizeof(sGen) - n, "[%c]", *p);
		}
	}
	else{
		size_t uGen = uSec1 - uTail;
		if(uGen >= sizeof(sGen)) uGen = sizeof(sGen) - 1;
		memcpy(sGen, sOrig, uGen);
		sGen[uGen] = '\0';
	}

	char sOut[5120] = {'\0'};
	if(sUnits[0] != '\0')
		snprintf(sOut, sizeof(sOut), "%s %s %s | %s", sGen, sElem, sUnits, pSec2);
	else
		snprintf(sOut, sizeof(sOut), "%s %s | %s", sGen, sElem, pSec2);

	return PyString_FromString(sOut);
}

/* A growable string for the rewritten dataset info */
typedef struct { char* p; size_t uLen; size_t uCap; } _pyd2_strbuf;

static bool _pyd2_sbAdd(_pyd2_strbuf* pSb, const char* s, size_t u)
{
	if(pSb->uLen + u + 1 > pSb->uCap){
		size_t uCap = 2*(pSb->uLen + u + 1);
		char* p = (char*)realloc(pSb->p, uCap);
		if(p == NULL) return false;
		pSb->p = p;
		pSb->uCap = uCap;
	}
	memcpy(pSb->p + pSb->uLen, s, u);
	pSb->uLen += u;
	pSb->p[pSb->uLen] = '\0';
	return true;
}

/* DasDs_info() with each "Var: <role> | <summary>" line taken from the
 * 'expression' that das2py put in that variable's dict, so the info string
 * says what python got.  Every other line is passed through.  Lines are
 * matched to dicts by the dimension header above them: "<Coordinate|Data>
 * Dimension: <name> (<physDim>)" */
static PyObject* _pyd2_dsInfo(
	const DasDs* pDs, PyObject* pCoordDict, PyObject* pDataDict
){
	int nInfo = DasDs_info(pDs, NULL, 0);
	char* sInfo = (nInfo >= 0) ? (char*)calloc(nInfo + 1, sizeof(char)) : NULL;
	if(sInfo == NULL) return PyErr_NoMemory();
	DasDs_info(pDs, sInfo, nInfo + 1);

	_pyd2_strbuf sb = {NULL, 0, 0};
	PyObject* pDim = NULL;   /* borrowed, dict of the dimension being listed */
	const char* sBad = NULL;
	const char* pLine = sInfo;

	while((*pLine != '\0') && (sBad == NULL)){
		const char* pEol = strchr(pLine, '\n');
		size_t uLine = (pEol != NULL) ? (size_t)(pEol - pLine + 1) : strlen(pLine);
		const char* p = pLine;
		while(*p == ' ') ++p;

		PyObject* pDict = NULL;
		const char* pName = NULL;
		if(strncmp(p, "Coordinate Dimension: ", 22) == 0){
			pDict = pCoordDict; pName = p + 22;
		}
		else if(strncmp(p, "Data Dimension: ", 16) == 0){
			pDict = pDataDict;  pName = p + 16;
		}

		if(pName != NULL){
			const char* pParen = strstr(pName, " (");
			char sName[256] = {'\0'};
			if((pParen == NULL) || ((size_t)(pParen - pName) >= sizeof(sName))){
				sBad = "dimension header"; break;
			}
			memcpy(sName, pName, pParen - pName);
			pDim = PyDict_GetItemString(pDict, sName);
			if(pDim == NULL){ sBad = "dimension name"; break; }
		}
		else if(strncmp(p, "Var: ", 5) == 0){
			const char* pRole = p + 5;
			const char* pBar = strstr(pRole, " | ");
			const char* pSp  = strchr(pRole, ' ');
			char sRole[64] = {'\0'};
			if((pBar == NULL) || (pSp == NULL) || (pDim == NULL) ||
			   ((pEol != NULL) && (pBar > pEol)) ||
			   ((size_t)(pSp - pRole) >= sizeof(sRole))){
				sBad = "variable line"; break;
			}
			memcpy(sRole, pRole, pSp - pRole);

			PyObject* pVar  = PyDict_GetItemString(pDim, sRole);
			PyObject* pExpr = pVar ? PyDict_GetItemString(pVar, "expression") : NULL;
			const char* sExpr = pExpr ? _pyd2_utf8(pExpr) : NULL;
			if(sExpr == NULL){ sBad = "variable role"; break; }

			if(!_pyd2_sbAdd(&sb, pLine, (pBar + 3) - pLine) ||
			   !_pyd2_sbAdd(&sb, sExpr, strlen(sExpr)) ||
			   ((pEol != NULL) && !_pyd2_sbAdd(&sb, "\n", 1))){
				free(sb.p); free(sInfo);
				return PyErr_NoMemory();
			}
			pLine += uLine;
			continue;
		}

		if(!_pyd2_sbAdd(&sb, pLine, uLine)){
			free(sb.p); free(sInfo);
			return PyErr_NoMemory();
		}
		pLine += uLine;
	}

	PyObject* pRet = NULL;
	if(sBad != NULL){
		PyErr_Clear();  /* a failed UTF-8 fetch may have set one */
		PyErr_Format(g_pPyD2Error,
			"Dataset %s info is not in the das v3.0 format, unexpected %s at: "
			"'%.80s'", DasDs_name(pDs), sBad, pLine
		);
	}
	else{
		pRet = PyString_FromString(sb.p ? sb.p : "");
	}
	free(sb.p);
	free(sInfo);
	return pRet;
}

/* {'kind': token, param: value, ...} for a variable's formalism, or None when
 * the variable carries no <ops>.  Keys are the wire attribute names, so what a
 * caller sees here is what they would read in the stream header. */
static PyObject* _form2PyDict(const DasVar* pVar)
{
	const DasForm* pForm = DasVar_form(pVar);
	if(pForm == NULL) Py_RETURN_NONE;

	PyObject* pOps = PyDict_New();
	if(pOps == NULL) return NULL;

	bool bOkay = true;
	/* For an unknown kind DasForm_kind() is the name as it arrived */
	bOkay = _setDictItem(pOps, "kind", PyString_FromString(DasForm_kind(pForm)));
	if(DasForm_isKind(pForm, DASFORM_EXT)){
		/* Every parameter of an unknown kind is a string, das2C can't know
		   better, so walk them all rather than the known list below */
		int nParams = DasForm_numParams(pForm);
		for(int i = 0; bOkay && (i < nParams); ++i){
			DasFormParamBuf buf;
			const DasProp* pParam = DasForm_paramAt(pForm, i, &buf);
			if(pParam == NULL) break;
			bOkay = _setDictItem(pOps, DasProp_name(pParam),
				PyString_FromString(DasProp_value(pParam)));
		}
	}
	else{
		for(int i = 0; bOkay && (g_sFormParams[i] != NULL); ++i){
			uint8_t uType = 0;
			const char* sVal = DasForm_param(pForm, g_sFormParams[i], &uType);
			if(sVal == NULL) continue;
			bOkay = _setDictItem(pOps, g_sFormParams[i], _formParam2Py(sVal, uType));
		}
	}

	if(!bOkay){ Py_DECREF(pOps); return NULL; }
	return pOps;
}

/* The internal shape as a list, empty for scalars.  A ragged level (a
 * variable length string) is None, since its extent is per value. */
static PyObject* _intrShape2PyList(const DasVar* pVar)
{
	ptrdiff_t aIntr[DASIDX_MAX] = {0};
	int nIntrRank = DasVar_intrShape(pVar, aIntr);

	PyObject* pList = PyList_New(nIntrRank);
	if(pList == NULL) return NULL;

	for(int i = 0; i < nIntrRank; ++i){
		PyObject* pItem = NULL;
		if(aIntr[i] == DASIDX_RAGGED){ Py_INCREF(Py_None); pItem = Py_None; }
		else pItem = PyLong_FromInt64(aIntr[i]);
		if(pItem == NULL){ Py_DECREF(pList); return NULL; }
		PyList_SET_ITEM(pList, i, pItem);  /* steals the reference */
	}
	return pList;
}

/* One label per component in storage order, one label for anything that is
 * not a composite.  The preference order (per component label property, then
 * a single label as a stem, then the dimension name) is das2C's, so output
 * from here matches what das3_cdf writes to LABL_PTR_1. */
static PyObject* _compLabels2PyList(const DasVar* pVar)
{
	long nComp = 1;
	if(strcmp(DasVar_serializeAs(pVar), "composite") == 0){
		ptrdiff_t aIntr[DASIDX_MAX] = {0};
		int nIntrRank = DasVar_intrShape(pVar, aIntr);
		for(int i = 0; i < nIntrRank; ++i) nComp *= aIntr[i];
	}

	const size_t uLenEa = 128;
	char** psBuf = (char**)calloc(nComp, sizeof(char*));
	char* pStore = (char*)calloc(nComp, uLenEa);
	if((psBuf == NULL)||(pStore == NULL)){
		free(psBuf); free(pStore);
		return PyErr_NoMemory();
	}
	for(long i = 0; i < nComp; ++i) psBuf[i] = pStore + i*uLenEa;

	PyObject* pList = NULL;
	int nGot = DasVar_compLbls(pVar, psBuf, (int)nComp, uLenEa);
	if(nGot < 0){
		PyErr_Format(g_pPyD2Error,
			"das2C error %d labeling the components of variable %s", -nGot,
			DasVar_serializeAs(pVar)
		);
	}
	else{
		pList = PyList_New(nGot);
		for(int i = 0; (pList != NULL) && (i < nGot); ++i){
			PyObject* pStr = PyString_FromString(psBuf[i]);
			if(pStr == NULL){ Py_DECREF(pList); pList = NULL; break; }
			PyList_SET_ITEM(pList, i, pStr);  /* steals the reference */
		}
	}

	free(psBuf); free(pStore);
	return pList;
}

/* Here's what we are going to output from each of the builder calls.
 * Right now it can only handle mapping square arrays.  Currently there
 * is no wrapper around DasVar, so no fancy operations are possible.
 *
 * All the structure keys start with '_'.  It is assumed that higher level
 * pure python code will handle reworking these raw dictionaries into something
 * slightly easier to access.
 *
 * Maybe like this:
 *
 *   time_at_index = d.coords['time']['center'][1, 27]
 *   freq_at_index = d.coords['freq']['center'][1, 27]
 *   ampl_at_index = d.coords['ampl']['center'][1, 27]
 *
 * To do this the variable class will need to be reimplemented on the python
 * side of the fence, but that should be easier and more useful than making a
 * C-wrapper around DasVar and keeping all data in our own arrays, even though
 * they can handle arbitrarily ragged items.
 *
 * d = {
 *   '_version': 	DasStream.version
 *   '_props':    DasStream.properties
 * }
 *
 * l =
 * [                        (list of dictionaries, 1 dict / dataset )
 *   {
 *    '_props':   DasDs.properties (Dictionary)
 *		'_rank':    DasDs.nRank,     (int)
 *		'_id':      DasDs.sId,       (string)
 *		'_group':   DasDs.sGroupId,  (string)
 *    'shape':    DasDs.shape,     (int tuple)
 *
 *		'_coords':  {         (Dictionary of coordinate dimensions)
 *       DasDim.sId: {
 *         '_id':        DasDim.sId
 *         '_props':      DasDim.properties (Dictionary)
 *         '_offset':    (None or some other dimension's name)
 *         '_type':      'COORD_DIM'
 *
 *          DasDim.aRole[i] : {
 *            '_role'     : DasDim.aRole[i]
 *            '_units'    : DasVar.units
 *            'array'     : backing array id, or None if computed
 *            'idxmap'    : [ Ds.nRank entries: array index or None ]
 *            'expression': [ concrete definition of variable, including arrays ]
 *            'ops'       : None, or {'kind': token, wire param: value, ...}
 *            'intern'    : [ internal shape, trailing the dataset indices ]
 *            'labels'    : [ one label per component, in storage order ]
 *           }
 *           ... (next variable)
 *        }
 *        ... (next dimension)
 *     }
 *
 *    '_data':  {         (Dictionary of coordinate dimensions)
 *       DasDim.sId: {
 *         '_props':      DasDim.properties (Dictionary)
 *         '_id':         DasDim.sId
 *         '_offset':     (None or some other dimension's name)
 *         '_type':       'DATA_DIM'
 *
 *         DasDim.aRole[i] : {
 *            '_role'  :  DasDim.aRole[i]
 *            '_units' : DasVar.units
 *            'expression' : [ concrete definition of variable includes arrays ]
 *            'array'  : sArray (key in array dictionary below), or None
 *            'idxmap' : [ Ds.nRank entries: array index or None ]
 *            'ops'    : None, or {'kind': token, wire param: value, ...}
 *            'intern' : [ internal shape, trailing the dataset indices ]
 *            'labels' : [ one label per component, in storage order ]
 *         }
 *         ... (next variable)
 *      }
 *      ... (next dimension)
 *    }
 *
 *	   'arrays': {
 *			sName : ndarray,
 *			sName : ndarray,
 *			sName : ndarray
 *		}
 *
 *		'fill' {
 *			sName : pyObj;
 *			sName : pyObj;
 *			sName : pyObj;
 *		}
 *  }
 *  ...
 * ]
 *
 *
 *  Higher level python code in the das2 module can take care of putting
 *  this dictionary into a dataset object
 */

static bool _addVars(
	const DasDs* pDs, DasDim* pDim, PyObject* pDimDict, PyObject* pdArys,
	PyObject* pdFill
){
	int nDsRank = DasDs_rank(pDs);
	DasVar* pVar = NULL;
	PyObject* pVarDict = NULL;
	PyObject* pStr = NULL;
	/* PyObject* pIdxMap = NULL; */
	/*	PyObject* pInt = NULL;*/
	
	size_t v=0;
	/* size_t i=0; */
	size_t uVars = DasDim_numVars(pDim);
	for(v = 0; v < uVars; ++v){
		pVar = DasDim_varAt(pDim, v);
		const char* sRole = DasDim_roleAt(pDim, v);
		pVarDict = PyDict_New();

		pStr = PyString_FromString(sRole);
		PyDict_SetItemString(pVarDict, "role", pStr);
		Py_DECREF(pStr);

		/* Array binding: stored arrays bind by id, sequences and constants
		 * are computed into a synthesized array, reference + offset gets
		 * None and is left to the python Dimension. */
		bool bBound = false;
		/* Neither array backed nor an operation: a sequence or a constant */
		bool bMaterial = !DasVar_hasAry(pVar) && !DasVar_isBin(pVar);
		if(bMaterial)
			bBound = _materialize(pDs, pDim, sRole, pVar, pVarDict, pdArys, pdFill);
		else
			bBound = _setDictItem(pVarDict, "array",  _varArrayId2Py(pVar)) &&
			         _setDictItem(pVarDict, "idxmap", _varIdxMap2PyList(pVar, nDsRank));
		if(!bBound){
			Py_DECREF(pVarDict);
			return false;
		}

		/* Units and summary as numpy will hold the values, the das2C
		 * variable keeps the stream's.  See _pyd2_varExpr(). */
		DasUnits units = _pyd2_npUnits(pVar);
		if(units == NULL) units = DasVar_units(pVar);
		if(!_setDictItem(pVarDict, "units", PyString_FromString(units ? units : "")) ||
		   !_setDictItem(pVarDict, "expression",
		                 _pyd2_varExpr(pDim, sRole, pVar, bMaterial))){
			Py_DECREF(pVarDict);
			return false;
		}

		/* Save the value type */
		pStr = PyString_FromString(_valTypeStr(DasVar_vt(pVar)));
		PyDict_SetItemString(pVarDict, "valtype", pStr);
		Py_DECREF(pStr);

		/* The <ops> formalism, the internal shape and the component labels.
		 * A scalar gets None, [] and a one item label list, so callers need
		 * not test for the keys. */
		if(!_setDictItem(pVarDict, "ops",    _form2PyDict(pVar)) ||
		   !_setDictItem(pVarDict, "intern", _intrShape2PyList(pVar)) ||
		   !_setDictItem(pVarDict, "labels", _compLabels2PyList(pVar))){
			Py_DECREF(pVarDict);
			return false;
		}


		/* Could save each sub-variable, don't know about this yet */
		/* pIdxMap = PyList_New(nDsRank);
		for(i = 0; i < nDsRank; ++i){
			pInt = PyInt_FromLong(pVar->idxmap[i]);
			PyList_SetItem(pIdxMap, i, pInt);
		}
		PyDict_SetItemString(pVarDict, "_idxmap", pIdxMap);
		Py_DECREF(pIdxMap);
		 */

		PyDict_SetItemString(pDimDict, sRole, pVarDict);
		Py_DECREF(pVarDict);
	}

	return true;
}


/* Takes in a DasStream object returns a 2-tuple of stream header plus datasets */
static PyObject* _Stream2Tuple(DasStream* pStream)
{
	DasDesc* pDesc = NULL;
	DasDs* pDs = NULL;
	DasDim* pDim = NULL;

	PyObject* pDsList = PyList_New( DasStream_numDesc(pStream, DASDESCTYPE_DS) );
	PyObject* pHdrDict = PyDict_New();

	/* Iteration variables used here */
	size_t d = 0; /* Dataset index */
	size_t a = 0; /* Array index */
	size_t m = 0; /* Dimension index */

	ptrdiff_t shape[DASIDX_MAX] = {0};
	int nRank, i = 0;
	DasAry* pDasAry = NULL;

	char sInfo[4096] = {'\0'};
	/* char sInfo[32768] = {'\0'}; */

	/* Before going through all the setup, see if the Das Arrays can even be
	 * converted to ndarrays with this extension.  Once code has been written
	 * to generate ragged ndarrays, remove this check */
	/* The V3 read model makes every descriptor a dataset, and the type
	 * filter on the walk would skip anything else anyway */
	pDesc = NULL;
	while((pDesc = DasStream_nextDesc(pStream, pDesc, DASDESCTYPE_DS)) != NULL){
		pDs = (DasDs*)pDesc;

		size_t uArys = DasDs_numArys(pDs);
		for(a = 0; a < uArys; ++a){
			pDasAry = DasDs_aryAt(pDs, a);

			/* Make sure the das array does not contain a user defined type */
			if(DasAry_vt(pDasAry) == DASVT_UNK){
				PyErr_Format(g_pPyD2Error,
					"Array %s from Dataset %s contains a generic type. "
					"Generic types are not supported by the python extension.",
					DasAry_toStr(pDasAry, sInfo, 63), DasDs_name(pDs)
				);
			}

			int nAryRank = DasAry_shape(pDasAry, shape);
			for(i = 1; i < nAryRank; ++i){

				if(shape[i] == DASIDX_RAGGED){

					/* Special exception if array is DASVT_BYTE, has the flag
					 * DASARY_AS_STRING and raggedness is only in the last dimension
					 * because I can turn these into cubic string object arrays */

					if((i == (nAryRank - 1)) &&
					   (DasAry_vt(pDasAry) == DASVT_BYTE) &&
						((DasAry_usage(pDasAry) & DASARY_AS_STRING)==DASARY_AS_STRING) )
						continue;

					PyErr_Format(g_pPyD2Error,
						"Array %s from dataset %s is ragged.  Conversion "
						"of ragged DasArrays to NumPy ndarrays has not been "
						"implemented.", DasAry_toStr(pDasAry, sInfo, 63), DasDs_name(pDs)
					);
					return NULL;
				}
			}
		}
		++d; // Increment the dataset index
	}
	memset(sInfo, 0, 64);

	PyObject* pDsDict = NULL;
	PyObject* pProps = NULL;
	PyObject* pStr = NULL;
	PyObject* pInt = NULL;
	PyObject* pCoordDict = NULL;
	PyObject* pDataDict = NULL;
	PyObject* pDict = NULL;
	PyObject* pDimDict = NULL;
	PyObject* pdArys = NULL;
	PyObject* pdFill = NULL;
	PyObject* pList = NULL;
	PyObject* pAry = NULL;
	PyObject* pObj = NULL;


	/* Handle the stream header conversion */
	pProps = _props2PyDict((DasDesc*)pStream);
	PyDict_SetItemString(pHdrDict, "props", pProps);
	DasStream_toInfoStr(pStream, sInfo, sizeof(sInfo));
	pStr = PyString_FromString(sInfo);
	PyDict_SetItemString(pHdrDict, "info", pStr);
	Py_DECREF(pStr);

	/* Note: PyDict_SetItem and PyList_SetItem handle reference counts
	 * differently.  Dicts increment the ref count when you hand them an
	 * item.  So if you don't want to own the item you have to decrement the
	 * count.  Lists just assume that you wanted them to own the item so they
	 * don't increment the count, but they do assume they can decrement it at
	 * will.  So... call Py_DECREF for dicts, but not for lists.  */

	pDesc = NULL;
	d = 0;
	while((pDesc = DasStream_nextDesc(pStream, pDesc, DASDESCTYPE_DS)) != NULL){
		pDs = (DasDs*)pDesc;

		/* Convert the properties */
		pProps = _props2PyDict((DasDesc*)pDs);
		if(pProps == NULL) return NULL;

		pDsDict = PyDict_New();
		PyDict_SetItemString(pDsDict, "props", pProps);
		Py_DECREF(pProps);

		pInt = PyInt_FromLong(DasDs_rank(pDs));
		PyDict_SetItemString(pDsDict, "rank", pInt);
		Py_DECREF(pInt);

		pStr = PyString_FromString(DasDs_name(pDs));
		PyDict_SetItemString(pDsDict, "id", pStr);
		Py_DECREF(pStr);

		pStr = PyString_FromString(DasDs_group(pDs) ? DasDs_group(pDs) : "");
		PyDict_SetItemString(pDsDict, "group", pStr);
		Py_DECREF(pStr);

		/* The info string is made last, from the finished variable dicts */
		
		nRank = DasDs_shape(pDs, shape);
		pList = PyList_New(nRank);
		for(i = 0; i < nRank; ++i){
			pInt = PyLong_FromInt64(shape[i]);
			PyList_SetItem(pList, i, pInt);
		}
		PyDict_SetItemString(pDsDict, "shape", pList);
		Py_DECREF(pList);

		/* Coordinate & Data dictionaries, we always have these even if they
		 * are empty, so just attach them now and dec my refs */
		pCoordDict = PyDict_New();
		PyDict_SetItemString(pDsDict, "coords", pCoordDict);
		Py_DECREF(pCoordDict);

		pDataDict = PyDict_New();
		PyDict_SetItemString(pDsDict, "data", pDataDict);
		Py_DECREF(pDataDict);

		/* Arrays and their fill values.  Made before the dimension walk since
		 * a computed variable adds a synthesized array of its own. */
		pdArys = PyDict_New();
		pdFill = PyDict_New();
		PyDict_SetItemString(pDsDict, "arrays", pdArys);
		PyDict_SetItemString(pDsDict, "fill",  pdFill);
		Py_DECREF(pdArys);
		Py_DECREF(pdFill);

		/* Dimensions are listed by type now, coordinates first.  Each type
		 * goes in its own dict, so the interleaving order v2 gave is moot. */
		static const das_dimtype aDimTypes[2] = {DASDIMTYPE_COORD, DASDIMTYPE_DATA};
		for(int t = 0; t < 2; ++t){
		size_t uDims = DasDs_numDims(pDs, aDimTypes[t]);
		for(m = 0; m < uDims; ++m){
			pDim = DasDs_dimAt(pDs, m, aDimTypes[t]);

			/* Create and add the dimension dictionary to the right category */
			pDimDict = PyDict_New();
			pDict = aDimTypes[t] == DASDIMTYPE_COORD ? pCoordDict : pDataDict;
			PyDict_SetItemString(pDict, DasDim_name(pDim), pDimDict);
			Py_DECREF(pDimDict);

			/* Fill in information for this dimension */
			pStr = PyString_FromString(
				aDimTypes[t] == DASDIMTYPE_COORD ? "COORD_DIM" : "DATA_DIM"
			);
			PyDict_SetItemString(pDimDict, "type", pStr);
			Py_DECREF(pStr);

			pProps = _props2PyDict((DasDesc*)pDim);
			if(pProps == NULL){ Py_DECREF(pDsDict); return NULL; }
			
			PyDict_SetItemString(pDimDict, "props", pProps);
			Py_DECREF(pProps);

			if( ! _addVars(pDs, pDim, pDimDict, pdArys, pdFill)){
				Py_DECREF(pDsDict); Py_DECREF(pDsList); return NULL;
			}
		}
		}  /* dimension types */

		size_t uArys = DasDs_numArys(pDs);
		for(a = 0; a < uArys; ++a){
			pDasAry = DasDs_aryAt(pDs, a);
			pAry = _DasAryToNumpyAry(pDasAry);
			if(pAry == NULL){
				Py_DECREF(pDsDict); Py_DECREF(pDsList);
				return NULL;
			}
			PyDict_SetItemString(pdArys, DasAry_id(pDasAry), pAry);
			Py_DECREF(pAry);

			pObj = _DasAryFillToObj(pDasAry);
			if(pObj == NULL){
				Py_DECREF(pDsDict); Py_DECREF(pDsList);
				return NULL;
			}
			PyDict_SetItemString(pdFill, DasAry_id(pDasAry), pObj);
			Py_DECREF(pObj);
		}
		
		/* The info string says what python got, so it's made from the
		 * finished variable dicts, see _pyd2_dsInfo() */
		pStr = _pyd2_dsInfo(pDs, pCoordDict, pDataDict);
		if(pStr == NULL){ Py_DECREF(pDsDict); Py_DECREF(pDsList); return NULL; }
		PyDict_SetItemString(pDsDict, "info", pStr);
		Py_DECREF(pStr);

PyList_SetItem(pDsList, d, pDsDict);

		++d; 
	}

	return Py_BuildValue("(OO)", pHdrDict, pDsList);
}

/* ************************************************************************* */
const char pyd2help_read_file[] =
"Reads a Das2 stream from a disk file and returns a stream header and a list\n"
"of DasDs (das dataset) objects containing the data in the stream\n"
"\n"
"Thread Note:  This function releases the global interpreter lock during stream\n"
"              reading\n"
"\n"
"Args:\n"
"   sFile (str) : The filename to read\n"
"\n"
"Return:\n"
"   A two-tuple consisting of a stream header dictionary and a list of correlated\n"
"   datasets.  The stream header is a dictionary with the following keys:\n"
"\n"
"     * 'props' - A list of dictionaries providing metadata about the overall stream\n"
"     * 'info'  - A summary string for the stream\n"
"\n"
"Each correlated dataset is a dictionary with the with the following keys and items:\n"
"\n"
"   * 'rank' - The number of array dimensions in each dataset\n"
"   * 'id'   - A string containing an identifier token usable as a variable name\n"
"   * 'group' - A string containing the join group for this Correlated dataset\n"
"   * 'shape' - An array containing the maximum index value in each dimension\n"
"   * 'coords' - A list of coordinate dictionaries (defined below)\n"
"   * 'datasets' - A list of datasets correlated in the given coordinates (see below)\n"
"   * 'arrays' - A dictionary of all the backing ndarrays for the dataset (see below)\n"
"   * 'props' - A list of dictionaries providing metadata about the dataset\n"
"   * 'info' - An information string about the dataset"
"\n"
"  Each item in 'coords' or 'data' is a dimension object that has the following keys\n"
"\n"
"   * 'type' - One of COORD_DIM or DATA_DIM\n"
"   * 'props' - A string containing the units of the coordinate\n"
"\n"
"  and one or more of the following optional keys:\n"
"\n"
"   * 'center' - A variable definition for data center values\n"
"   * 'reference' - A variable definition for data reference point (ususally start) values\n"
"   * 'offset' - A variable definition for data offset value, to be added to reference\n"
"\n"
"  Other variable definitions may follow for min, max, stddev etc. values in a dimension\n"
"\n"
"  Each variable definition is a dictionary with the keys:\n"
"\n"
"   * 'role' - The variable's role in its dimension (center, reference, ...)\n"
"   * 'units' - The units string, after conversion of times to nanoseconds\n"
"   * 'expression' - A human readable description of the variable\n"
"   * 'valtype' - The das2C value type name\n"
"   * 'array' - The key in 'arrays' of the ndarray backing this variable.  A\n"
"               sequence or constant is computed into an array named dim.role;\n"
"               reference + offset is None, the dimension derives it.\n"
"   * 'idxmap' - One entry per dataset index giving the array index it maps to,\n"
"                None where the variable does not vary.  None as a whole when\n"
"                'array' is None.\n"
"   * 'ops' - None, or a dictionary of the variable's <ops> formalism: 'kind'\n"
"             plus each parameter under its wire attribute name (frame, body,\n"
"             fixed, system, sysorder, surface, from, to).  Unknown kinds carry\n"
"             their parameters verbatim.\n"
"   * 'intern' - The internal shape of one value as a list, empty for scalars;\n"
"                None marks a ragged level such as a variable length string\n"
"   * 'labels' - One label per component in storage order, or one label for a\n"
"                scalar.  Same preference order as das3_cdf's LABL_PTR_1\n"
"\n";

/* Read a whole stream from a transport into python objects.  Takes the
 * transport, NULL included, so callers need no cleanup of their own.
 *
 * The v2 dataset builder is gone: a reader in the V3 model up-converts das2
 * headers to datasets itself, and with accumulation on keeps every packet,
 * so the stream it hands over is what the builder used to assemble. */
static PyObject* _readXport(DasXport* pX)
{
	if(pX == NULL) return pyd2_setException(g_pPyD2Error);

	DasIO* pIn = new_DasIO("das3py", pX, "r");
	if(pIn == NULL){
		del_DasXport(pX);
		return pyd2_setException(g_pPyD2Error);
	}
	DasIO_setModel(pIn, DASIOMODEL_V3);
	DasIO_setAccum(pIn, true);

	int nRet = DAS_OKAY;

	/* Release the GIL while doing I/O */
	Py_BEGIN_ALLOW_THREADS
	nRet = DasIO_readAll(pIn);
	Py_END_ALLOW_THREADS

	if(nRet != DAS_OKAY){
		del_DasIO(pIn);
		return pyd2_setException(g_pPyD2Error);
	}

	DasStream* pStream = DasIO_takeStream(pIn);
	del_DasIO(pIn);
	if(pStream == NULL){
		/* v2 returned NULL here with no exception set */
		PyErr_SetString(g_pPyD2Error, "Input ended before a stream header was read");
		return NULL;
	}

	PyObject* pRet = _Stream2Tuple(pStream);
	del_DasDesc((DasDesc*)pStream);  /* arrays numpy took over were disowned */
	return pRet;
}

static PyObject* pyd2_read_file(PyObject* self, PyObject* args)
{
	const char* sFile;

	if(!PyArg_ParseTuple(args, "s:read_file", &sFile))
		return NULL;

	return _readXport(new_DasXport_file(sFile, "r"));
}

/* ************************************************************************* */
static const char pyd2help_read_server[] =
"read_server(sUrl, sAgent=None)\n"
"\n"
"Reads a Das2 stream from a remote HTTP/HTTPS server.\n"
"\n"
"Note:\n"
"   This function releases the global interpreter lock during data download\n"
"\n"
"Args:\n"
"   sUrl (str) : The URL to read, can be an extensive GET string\n"
"   rConSec (float, optional) : How long to wait on the connection to the\n"
"      remote server in seconds.  A value of <= 0.0 means wait as long as\n"
"      the operating system allows."
"   sAgent (str,optional) : The user agent string you'd like to use\n"
"\n"
"Returns:\n"
"   This function has the same return as :ref:`read_file`.\n"
"\n"
;

/* Read in das2 stream from a server */
static PyObject* pyd2_read_server(PyObject* self, PyObject* args)
{
	const char* sInitialUrl = "https://planet.physics.uiowa.edu/das/das2Server"
	       "?server=dataset&dataset=Galileo/PWS/Survey_Electric"
	       "&start_time=2001-001&end_time=2001-002";
	const char* sUserAgent = NULL;
	float rConSec = DASHTTP_TIMEOUT_MIN * DASHTTP_TIMEOUT_MULTI;
	if(!PyArg_ParseTuple(args, "s|fs:read_server", &sInitialUrl, &rConSec,
			               &sUserAgent))
		return NULL;

	PyObject* pExcept = g_pPyD2Error;
	DasHttpResp* pRes = NULL;

	/* Release the GIL while the connection processes */
	Py_BEGIN_ALLOW_THREADS
	pRes = new_DasHttpResp_get(sInitialUrl, sUserAgent, g_pMgr, rConSec);
	Py_END_ALLOW_THREADS

	if(pRes == NULL) return PyErr_NoMemory();

	if(!DasHttpResp_isOk(pRes)){
		int nCode = DasHttpResp_code(pRes);
		if((nCode == 401)||(nCode == 403)) pExcept = g_pPyD2AuthErr;
		if((nCode == 400)||(nCode == 404)) pExcept = g_pPyD2QueryErr;
		if(pExcept == NULL) pExcept = g_pPyD2Error;

		const char* sErr = DasHttpResp_error(pRes);
		PyErr_Format(pExcept, "%d, Could not get body for URL, reason: %s",
		             nCode, sErr ? sErr : "unknown");
		del_DasHttpResp(pRes);
		return NULL;
	}

	if(strcmp(DasHttpResp_url(pRes), sInitialUrl) != 0)
		daslog_infof("Redirected to %s", DasHttpResp_url(pRes));

	/* The body transport hides whether this is a socket or TLS */
	DasXport* pBody = DasHttpResp_takeBody(pRes);
	del_DasHttpResp(pRes);
	return _readXport(pBody);
}


/* ************************************************************************* */
const char pyd2help_read_cmd[] =
"read_cmd(sCmd)\n"
"\n"
"Reads a Das2 stream from an external program and returns a list of dictionaries\n"
"that describe dataset and hold the NumPy arrays containing the data.\n"
"\n"
"Note:\n"
"   This function releases the global interpreter lock during data download\n"
"\n"
"Args:\n"
"   sCmd (str) : The reader command line to run.  Standard output from the\n"
"      command is expected to be a das2 stream."
"\n"
"Returns:\n"
"   This function has the same return as :ref:`read_file`.\n"
"\n"
;

static PyObject* pyd2_read_cmd(PyObject* self, PyObject* args)
{

	const char* sCmd;

	if(!PyArg_ParseTuple(args, "s:read_cmd", &sCmd))
		return NULL;

	return _readXport(new_DasXport_cmd(sCmd));
}
