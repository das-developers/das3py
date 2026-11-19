/* Copyright (C) 2018-2026 Chris Piker
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

#ifndef _WIN32
#include <strings.h>
#endif

#include <Python.h>

#include <das3/http.h>
#include <das3/node.h>
#include <das3/catalog.h>
#include <das3/str.h>
#include <das3/log.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

/* This block is just here to make IDE code assistance not throw up lots of 
 * errors since it has to be able to understand how to parse this file on 
 * it's own. */
#ifndef HAS_DAS2_PYEXCEPT
static PyObject* g_pPyD2Error;
static void* pyd2_setException(PyObject* pExcept);
static void* pyd2_setExceptFromLog(PyObject* pExcept);
static PyObject* g_pPyD2QueryErr;
static PyObject* g_pPyD2AuthErr;
static DasCredMgr* g_pMgr = NULL;
#endif

/* ************************************************************************* */
/* converting JSON data to python objects                                    */
PyObject* DasJdo_toPyObj(const DasJdo* pJdo){
	
	PyObject* pPy = NULL;
	PyObject* pPySub = NULL;
	const char* sStr = NULL;
	const DasJsonDictEl* pDel = NULL;
	const DasJsonAryEl* pAel = NULL;
	const DasJsonStr* pS = NULL;
	const DasJsonNum* pN = NULL;
	double rNum = 0.0;
	Py_ssize_t u = 0;
	
	/* Note: PyDict_SetItem and PyList_SetItem handle reference counts 
	 * differently.  Dicts increment the ref count when you hand them an 
	 * item.  So if you don't want to own the item you have to decrement the
	 * count.  Lists just assume that you wanted them to own the item so they
	 * don't increment the count, but they do assume they can decrement it at
	 * will.  So... call Py_DECREF for dicts, but not for lists.  */
	
	switch(pJdo->type){
	case DASJSONTYPE_DICT:
		pPy = PyDict_New();
		if(pPy == NULL) 
			return NULL;
		
		for(pDel = DasJdo_dictFirst(pJdo); pDel != NULL; pDel = pDel->next){
			sStr = ((DasJsonStr*)pDel->name)->string;
			if(sStr != NULL){
				pPySub = DasJdo_toPyObj( pDel->value);
				if(pPySub == NULL){ 
					Py_DECREF(pPy); return NULL; 
				}
				if(PyDict_SetItemString(pPy, sStr, pPySub) != 0){
					Py_DECREF(pPySub); Py_DECREF(pPy); return NULL; 
				}
			}
		}
		return pPy;
		
	case DASJSONTYPE_ARY:
		pPy = PyList_New( ((DasJsonAry*)(pJdo->value))->length );
		if(pPy == NULL) 
			return NULL;
		
		u = 0;
		for(pAel = DasJdo_aryFirst(pJdo); pAel != NULL; pAel = pAel->next){
			pPySub = DasJdo_toPyObj(pAel->value);
			if((pPySub == NULL)||(PyList_SetItem(pPy, u, pPySub) != 0)){
				Py_DECREF(pPy); return NULL;
			}
			++u;
		}
		return pPy;
		
	case DASJSONTYPE_STR:
		pS = (DasJsonStr*)pJdo->value;
		pPy = PyString_FromString(pS->string);
		return pPy;
		
	case DASJSONTYPE_NUM:
		/* Return an integer if you can */
		pN = (DasJsonNum*)pJdo->value;
		if(!dasstr_toDouble(pN->number, &rNum)){
			daslog_errorf("Couldn't convert %s to a number", pN->number);
			return NULL;
		}
		if( rNum == (long int) rNum)
			return PyInt_FromLong((long int)rNum);  // Convert as integer
		else
			return PyFloat_FromDouble(rNum);        // Convert as double
		
		
		
	case DASJSONTYPE_TRUE:
		return PyBool_FromLong(1);
		
	case DASJSONTYPE_FALSE:
		return PyBool_FromLong(0);
		
	case DASJSONTYPE_NULL:
		Py_RETURN_NONE;
	}
	return pPy;
}

/* ************************************************************************* */
/* Reading catalog data */

/* One catalog session for the life of the module.  It keeps every node it
 * loads, so walking the same branch twice in a process costs nothing.  It
 * keeps nodes in memory only; das2C's disk cache is opt-in as of v3.0.5 and
 * a library has no business writing under the user's home directory unasked,
 * so it's never turned on here.  It takes ownership of g_pMgr; the module
 * never frees either, so py_builder
 * keeps using g_pMgr for data requests.  The user agent of the first call
 * sticks, as it did for the v2 root node. */
static DasCat* g_pCat = NULL;

static char pyd2help_get_node[] = 
"Get JSON data from a das2 Catalog Node by URL or URI.\n"
"\n"
"Arguments\n"
"   path_uri (string) - The URI of the node to load.  By default the federated\n"
"         catalog system is used to provide the URL to the catalog node file.\n"
"\n"
"   agent (string, optional) - If present (and not None) then the supplied text\n"
"         will be the User Agent string supplied to any remote HTTP servers that\n"
"         are contacted.\n"
"\n"
"   url (string, optional) - If present (and not None) then the node is loaded\n"
"         directly from the URL provided, potentially skipping the global\n"
"         catalog, this is useful for testing stand alone catalog files\n";

/* The node as a dict, plus the "_url" and "_path" keys node.py relies on.
 * A root has no URI; v2 reported "" for it and container.py composes
 * child paths from it, so keep "". */
static PyObject* _pyd2_nodeToDict(const DasNode* pNode)
{
	if(!DasNode_isJson(pNode)){
		PyErr_SetString(PyExc_NotImplementedError, 
			"Handling non-JSON catalogs is not yet implemented"
		);
		return NULL;
	}
		
	PyObject* pPyObj = DasJdo_toPyObj(DasNode_jdo(pNode, NULL));
	if(pPyObj == NULL){
		return pyd2_setExceptFromLog(g_pPyD2Error);
	}
	
	/* Save the URL used to get the item, this is very handy for informational 
	 * messages */
	PyObject* pTmp = PyString_FromString(DasNode_srcUrl(pNode));
	int nRet = PyDict_SetItemString(pPyObj, "_url", pTmp);
	Py_DECREF(pTmp);
	if(nRet != 0){ Py_DECREF(pPyObj); return NULL; }
	
	const char* sPath = DasNode_pathUri(pNode);
	pTmp = PyString_FromString(sPath ? sPath : "");
	nRet = PyDict_SetItemString(pPyObj, "_path", pTmp);
	Py_DECREF(pTmp);
	if(nRet != 0){ Py_DECREF(pPyObj); return NULL; }

	return pPyObj;
}

/* A failed resolve logs nothing when the path simply isn't there, so say
 * where the walk stopped, as das_fetch does.  When the child is listed the
 * load itself failed and the log usually has the reason, so prefer that. */
static PyObject* _pyd2_resolveFailed(DasCat* pCat)
{
	const char* sRest = NULL;
	DasNode* pNear = DasCat_nearest(pCat, &sRest);
	if((pNear == NULL)||(sRest == NULL))
		return pyd2_setExceptFromLog(g_pPyD2Error);

	const char* sParent = DasCat_lastUri(pCat);  /* names pNear after nearest() */

	char sChild[128] = {'\0'};
	snprintf(sChild, sizeof(sChild), "%s", sRest);
	char* pSlash = strchr(sChild, '/');
	if(pSlash != NULL) *pSlash = '\0';

	size_t uKids = DasNode_numSubs(pNear);
	bool bListed = false;
	for(size_t u = 0; u < uKids; ++u)
		if(strcasecmp(DasNode_subNameAt(pNear, u), sChild) == 0){ bListed = true; break; }

	if(bListed && (g_sLogBuf[0] != '\0'))
		return pyd2_setExceptFromLog(g_pPyD2Error);

	char sMsg[D2PY_LOG_BUF_SZ];
	size_t n = (size_t)snprintf(sMsg, sizeof(sMsg),
		bListed ? "'%s' under %s is listed but could not be loaded" :
		          "No child named '%s' under %s",
		sChild, (sParent && sParent[0]) ? sParent : "the catalog root"
	);
	if(uKids > 0 && n < sizeof(sMsg)){
		n += snprintf(sMsg + n, sizeof(sMsg) - n, ".  Children:");
		for(size_t u = 0; (u < uKids) && (n < sizeof(sMsg)); ++u)
			n += snprintf(sMsg + n, sizeof(sMsg) - n, " %s", DasNode_subNameAt(pNear, u));
	}
	memset(g_sLogBuf, 0, D2PY_LOG_BUF_SZ);
	PyErr_SetString(g_pPyD2Error, sMsg);
	return NULL;
}

static PyObject* pyd2_get_node(PyObject* self, PyObject* args)
{
	
	const char* sUri = NULL;
	const char* sAgent = NULL;
	const char* sLocation = NULL;
	DasNode* pNode = NULL;
	
	if(!PyArg_ParseTuple(args, "z|zz:get_node", &sUri, &sAgent, &sLocation))
		return NULL;

	/* A node by location, named sUri.  Mounts are refused once a session has
	   resolved anything, so this can't go through g_pCat; a throw-away
	   session seeded with the location gives the node that name.  It can't
	   borrow g_pMgr, since a session frees its manager, so these loads are
	   public only, which suits the test files this is for. */
	if(sLocation != NULL){
		DasCat* pTmp = new_DasCat(NULL, sAgent);
		if(pTmp == NULL) return pyd2_setExceptFromLog(g_pPyD2Error);

		PyObject* pRet = NULL;
		if(DasCat_setSeed(pTmp, sLocation, sUri) == DAS_OKAY){
			pNode = DasCat_seed(pTmp);
			if(pNode != NULL) pRet = _pyd2_nodeToDict(pNode);
			else pyd2_setExceptFromLog(g_pPyD2Error);
		}
		else{
			pyd2_setExceptFromLog(g_pPyD2Error);
		}
		del_DasCat(pTmp);   /* the dict holds copies, node strings go here */
		return pRet;
	}
	
	if(g_pCat == NULL){
		g_pCat = new_DasCat(g_pMgr, sAgent);
		if(g_pCat == NULL) return pyd2_setExceptFromLog(g_pPyD2Error);
	}
	
	if(sUri == NULL)
		pNode = DasCat_seed(g_pCat);  /* the das catalog index, a root */
	else
		pNode = DasCat_resolve(g_pCat, sUri);
	
	if(pNode == NULL){
		if(sUri == NULL) return pyd2_setExceptFromLog(g_pPyD2Error);
		return _pyd2_resolveFailed(g_pCat);
	}

	return _pyd2_nodeToDict(pNode);
}
