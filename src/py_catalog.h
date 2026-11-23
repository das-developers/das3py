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
#include <das3/source.h>
#include <das3/req.h>
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
 * keeps using g_pMgr for data requests. */
static DasCat* g_pCat = NULL;

/* The module's session, made on first use.  NULL with an exception set if
 * it can't be made.
 *
 * An agent given here is used from now on, not only when the session is
 * made: das3's import-time mount() creates the session before any caller
 * can name one, so "first agent sticks" meant the caller's was dropped. */
static DasCat* _pyd2_catalog(const char* sAgent)
{
	if(g_pCat == NULL){
		g_pCat = new_DasCat(g_pMgr, sAgent);
		if(g_pCat == NULL) pyd2_setExceptFromLog(g_pPyD2Error);
	}
	else if(sAgent != NULL){
		DasCat_setAgent(g_pCat, sAgent);
	}
	return g_pCat;
}

/* A lookup failed and das2C said why in the das_catmiss it was handed.
 * Given a struct, das2C logs nothing about the lookup itself, so the
 * message is the report.  Whatever the walk did log on the way, an HTTP
 * warning for instance, is superseded by it and dropped, else a later
 * failure that logs nothing would be raised with that stale text.  The
 * log is only the fallback for a failure that left no message. */
static PyObject* _pyd2_setExceptFromMiss(const das_catmiss* pMiss)
{
	if(pMiss->sMsg[0] == '\0') return pyd2_setExceptFromLog(g_pPyD2Error);
	memset(g_sLogBuf, 0, D2PY_LOG_BUF_SZ);
	PyErr_SetString(g_pPyD2Error, pMiss->sMsg);
	return NULL;
}

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

static PyObject* pyd2_get_node(PyObject* self, PyObject* args)
{
	
	const char* sUri = NULL;
	const char* sAgent = NULL;
	const char* sLocation = NULL;
	DasNode* pNode = NULL;
	
	if(!PyArg_ParseTuple(args, "z|zz:get_node", &sUri, &sAgent, &sLocation))
		return NULL;

	das_catmiss miss;

	/* A node by location, named sUri.  Going through g_pCat would take a
	   mount, and the mount would stay and shadow that name for the rest of
	   the process.  A throw-away session seeded with the location gives the
	   node its name with no such side effect.  It can't borrow g_pMgr, since
	   a session frees its manager, so these loads are public only, which
	   suits the test files this is for. */
	if(sLocation != NULL){
		DasCat* pTmp = new_DasCat(NULL, sAgent);
		if(pTmp == NULL) return pyd2_setExceptFromLog(g_pPyD2Error);

		PyObject* pRet = NULL;
		if(DasCat_setSeed(pTmp, sLocation, sUri) == DAS_OKAY){
			pNode = DasCat_seed(pTmp, &miss);
			if(pNode != NULL) pRet = _pyd2_nodeToDict(pNode);
			else _pyd2_setExceptFromMiss(&miss);
		}
		else{
			pyd2_setExceptFromLog(g_pPyD2Error);   /* a malformed argument, logged */
		}
		del_DasCat(pTmp);   /* the dict holds copies, node strings go here */
		return pRet;
	}
	
	if(_pyd2_catalog(sAgent) == NULL) return NULL;
	
	if(sUri == NULL)
		pNode = DasCat_seed(g_pCat, &miss);  /* where paths start, a root by default */
	else
		pNode = DasCat_resolve(g_pCat, sUri, &miss);
	
	if(pNode == NULL) return _pyd2_setExceptFromMiss(&miss);

	return _pyd2_nodeToDict(pNode);
}

/* ************************************************************************* */
/* Data sources.  das2C reads the source node and builds requests from it,
 * in both the 2018 and the current node layouts; das3py only asks it
 * questions.  So a fix or a new layout reaches python with a rebuild.
 *
 * Every function takes the source by catalog path, URL or file name and
 * builds a DasSrc from the session's cached node for the length of the
 * call.  That copies the node's JSON, which is cheap next to the fetch
 * that follows, and saves python from managing a C object. */

/* NULL with an exception set when the path doesn't lead to a source.  The
 * message is das2C's: for a path that ends at a container it names the
 * children that can be loaded. */
static DasSrc* _pyd2_newSrc(const char* sPath)
{
	if(_pyd2_catalog(NULL) == NULL) return NULL;
	das_catmiss miss;
	DasSrc* pSrc = new_DasSrc_cat(g_pCat, sPath, &miss);
	if(pSrc == NULL) _pyd2_setExceptFromMiss(&miss);
	return pSrc;
}

/* A str, or None for NULL.  Many getters answer NULL for "not stated". */
static PyObject* _pyd2_strOrNone(const char* s)
{
	if(s == NULL) Py_RETURN_NONE;
	return PyString_FromString(s);
}

/* Append and give up the reference either way.  False with an exception
 * set on failure, including a NULL item. */
static bool _pyd2_append(PyObject* pList, PyObject* pItem)
{
	if(pItem == NULL) return false;
	int nRet = PyList_Append(pList, pItem);
	Py_DECREF(pItem);
	return (nRet == 0);
}

/* The library's folded type.  The node's own spelling, which drifts over
 * the years, goes in 'type' beside it. */
static const char* _pyd2_propKind(das_srcproptype et)
{
	switch(et){
	case DASSRCPROPTYPE_BOOL: return "bool";
	case DASSRCPROPTYPE_INT:  return "int";
	case DASSRCPROPTYPE_REAL: return "real";
	case DASSRCPROPTYPE_TIME: return "time";
	case DASSRCPROPTYPE_ENUM: return "enum";
	default:                  return "text";
	}
}

static PyObject* _pyd2_propToDict(const DasSrcProp* pProp)
{
	PyObject* pDict = PyDict_New();
	if(pDict == NULL) return NULL;

	char sTitle[256] = {'\0'};
	const DasSrcGroup* pGrp = DasSrcProp_group(pProp);
	const DasSrcCtrl* pCtrl = DasSrcProp_ctrl(pProp);

	bool bOk =
		_setDictItem(pDict, "key",     PyString_FromString(DasSrcProp_key(pProp))) &&
		_setDictItem(pDict, "section", PyString_FromString(dassrcsec_name(DasSrcProp_section(pProp)))) &&
		_setDictItem(pDict, "group",   _pyd2_strOrNone(pGrp ? DasSrcGroup_name(pGrp) : NULL)) &&
		_setDictItem(pDict, "name",    PyString_FromString(DasSrcProp_name(pProp))) &&
		_setDictItem(pDict, "title",   PyString_FromString(DasSrcProp_title(pProp, sTitle, sizeof(sTitle)))) &&
		_setDictItem(pDict, "kind",    PyString_FromString(_pyd2_propKind(DasSrcProp_type(pProp)))) &&
		_setDictItem(pDict, "type",    PyString_FromString(DasSrcProp_typeName(pProp))) &&
		_setDictItem(pDict, "units",   PyString_FromString(DasSrcProp_units(pProp))) &&
		_setDictItem(pDict, "value",   _pyd2_strOrNone(DasSrcProp_value(pProp))) &&
		_setDictItem(pDict, "settable", PyBool_FromLong(pCtrl != NULL));
	if(!bOk){ Py_DECREF(pDict); return NULL; }

	/* A list property: (fewest, most) items when a list is given, most is
	   None when the node sets no bound.  None for an ordinary property.
	   'kind', 'type', 'enum' and 'range' then describe one item, and
	   'value' is the default items joined by commas. */
	size_t uLo = 0, uHi = 0;
	PyObject* pMulti = NULL;
	if(DasSrcProp_multi(pProp, &uLo, &uHi)){
		if(uHi == SIZE_MAX) pMulti = Py_BuildValue("(nO)", (Py_ssize_t)uLo, Py_None);
		else pMulti = Py_BuildValue("(nn)", (Py_ssize_t)uLo, (Py_ssize_t)uHi);
	}
	else{
		Py_INCREF(Py_None);
		pMulti = Py_None;
	}
	if(!_setDictItem(pDict, "multi", pMulti)){ Py_DECREF(pDict); return NULL; }

	/* What a form needs to offer a value, only for adjustable properties */
	PyObject* pEnum = NULL;
	PyObject* pRange = NULL;
	if(pCtrl != NULL){
		size_t uEnums = DasSrcCtrl_numEnums(pCtrl);
		pEnum = PyList_New(0);
		if(pEnum == NULL){ Py_DECREF(pDict); return NULL; }
		for(size_t u = 0; u < uEnums; ++u){
			if(!_pyd2_append(pEnum, _pyd2_strOrNone(DasSrcCtrl_enumAt(pCtrl, u)))){
				Py_DECREF(pEnum); Py_DECREF(pDict); return NULL;
			}
		}
		const char* sLo = NULL;
		const char* sHi = NULL;
		if(DasSrcCtrl_range(pCtrl, &sLo, &sHi))
			pRange = Py_BuildValue("(NN)", _pyd2_strOrNone(sLo), _pyd2_strOrNone(sHi));
		else{
			Py_INCREF(Py_None);
			pRange = Py_None;
		}
		bOk =
			_setDictItem(pDict, "required", PyBool_FromLong(DasSrcCtrl_isRequired(pCtrl))) &&
			_setDictItem(pDict, "param",    PyString_FromString(DasSrcCtrl_param(pCtrl))) &&
			_setDictItem(pDict, "enum",     pEnum) &&
			_setDictItem(pDict, "range",    pRange);
	}
	else{
		Py_INCREF(Py_None); Py_INCREF(Py_None);
		bOk =
			_setDictItem(pDict, "required", PyBool_FromLong(0)) &&
			_setDictItem(pDict, "param",    PyString_FromString("")) &&
			_setDictItem(pDict, "enum",     Py_None) &&
			_setDictItem(pDict, "range",    Py_None);
	}
	if(!bOk){ Py_DECREF(pDict); return NULL; }
	return pDict;
}

static PyObject* _pyd2_groupToDict(const DasSrcGroup* pGrp)
{
	PyObject* pDict = PyDict_New();
	if(pDict == NULL) return NULL;

	PyObject* pMimes = PyList_New(0);
	if(pMimes == NULL){ Py_DECREF(pDict); return NULL; }
	size_t uMimes = DasSrcGroup_numMimes(pGrp);
	for(size_t u = 0; u < uMimes; ++u){
		if(!_pyd2_append(pMimes, _pyd2_strOrNone(DasSrcGroup_mimeAt(pGrp, u)))){
			Py_DECREF(pMimes); Py_DECREF(pDict); return NULL;
		}
	}
	const DasSrcProp* pOn = DasSrcGroup_enableProp(pGrp);

	bool bOk =
		_setDictItem(pDict, "section", PyString_FromString(dassrcsec_name(DasSrcGroup_section(pGrp)))) &&
		_setDictItem(pDict, "name",    PyString_FromString(DasSrcGroup_name(pGrp))) &&
		_setDictItem(pDict, "label",   PyString_FromString(DasSrcGroup_label(pGrp))) &&
		_setDictItem(pDict, "title",   PyString_FromString(DasSrcGroup_title(pGrp))) &&
		_setDictItem(pDict, "enable",  _pyd2_strOrNone(pOn ? DasSrcProp_key(pOn) : NULL)) &&
		_setDictItem(pDict, "xor",     _pyd2_strOrNone(DasSrcGroup_xorGroup(pGrp))) &&
		_setDictItem(pDict, "mimes",   pMimes);
	if(!bOk){ Py_DECREF(pDict); return NULL; }
	return pDict;
}

/* Everything a python Source needs to describe itself and offer choices */
static PyObject* _pyd2_srcToDict(const DasSrc* pSrc)
{
	PyObject* pDict = PyDict_New();
	if(pDict == NULL) return NULL;
	PyObject* pList = NULL;
	size_t u, uN;

	bool bOk =
		_setDictItem(pDict, "path",       PyString_FromString(DasSrc_path(pSrc))) &&
		_setDictItem(pDict, "url",        PyString_FromString(DasSrc_url(pSrc))) &&
		_setDictItem(pDict, "name",       PyString_FromString(DasSrc_name(pSrc))) &&
		_setDictItem(pDict, "title",      PyString_FromString(DasSrc_title(pSrc))) &&
		_setDictItem(pDict, "version",    PyString_FromString(DasSrc_version(pSrc))) &&
		_setDictItem(pDict, "needs_auth", PyBool_FromLong(DasSrc_needsAuth(pSrc))) &&
		_setDictItem(pDict, "auth_realm", PyString_FromString(DasSrc_authRealm(pSrc)));
	if(!bOk) goto FAIL;

	/* Contacts, as (name, email) with "" for no address */
	if((pList = PyList_New(0)) == NULL) goto FAIL;
	uN = DasSrc_numContacts(pSrc);
	for(u = 0; u < uN; ++u){
		const char* sEmail = NULL;
		const char* sName = DasSrc_contactAt(pSrc, u, &sEmail);
		if(sName == NULL) break;
		if(!_pyd2_append(pList, Py_BuildValue("(ss)", sName, sEmail ? sEmail : "")))
			goto FAIL;
	}
	if(!_setDictItem(pDict, "contacts", pList)){ pList = NULL; goto FAIL; }

	/* Mirrors, tried in order by get() */
	if((pList = PyList_New(0)) == NULL) goto FAIL;
	uN = DasSrc_numBaseUrls(pSrc);
	for(u = 0; u < uN; ++u)
		if(!_pyd2_append(pList, _pyd2_strOrNone(DasSrc_baseUrlAt(pSrc, u)))) goto FAIL;
	if(!_setDictItem(pDict, "base_urls", pList)){ pList = NULL; goto FAIL; }

	/* Examples with the MIME type each produces, so python can pass over
	 * the ones it can't parse, a CDF download for instance */
	if((pList = PyList_New(0)) == NULL) goto FAIL;
	uN = DasSrc_numExamples(pSrc);
	for(u = 0; u < uN; ++u){
		DasReq* pReq = new_DasReq_src(pSrc, (int)u);
		const char* sMime = (pReq == NULL) ? "" : DasSrc_mime(pSrc, pReq);
		PyObject* pEx = Py_BuildValue("{s:s,s:s}",
			"label", DasSrc_exampleLblAt(pSrc, u), "mime", sMime ? sMime : ""
		);
		if(pReq != NULL) del_DasReq(pReq);
		if(!_pyd2_append(pList, pEx)) goto FAIL;
	}
	if(!_setDictItem(pDict, "examples", pList)){ pList = NULL; goto FAIL; }

	/* Groups by section, then every property in presentation order */
	if((pList = PyList_New(0)) == NULL) goto FAIL;
	for(int sec = 0; sec < DASSRCSEC_SIZE; ++sec){
		uN = DasSrc_numGroups(pSrc, (das_srcsec)sec);
		for(u = 0; u < uN; ++u)
			if(!_pyd2_append(pList, _pyd2_groupToDict(DasSrc_groupAt(pSrc, (das_srcsec)sec, u))))
				goto FAIL;
	}
	if(!_setDictItem(pDict, "groups", pList)){ pList = NULL; goto FAIL; }

	if((pList = PyList_New(0)) == NULL) goto FAIL;
	uN = DasSrc_numProps(pSrc);
	for(u = 0; u < uN; ++u)
		if(!_pyd2_append(pList, _pyd2_propToDict(DasSrc_propAt(pSrc, u)))) goto FAIL;
	if(!_setDictItem(pDict, "props", pList)){ pList = NULL; goto FAIL; }

	return pDict;

FAIL:
	Py_XDECREF(pList);
	Py_DECREF(pDict);
	return NULL;
}

static char pyd2help_src_info[] =
"Describe a data source as das2C reads it.\n"
"\n"
"Arguments\n"
"   path (string) - The source's catalog path, or the URL or file name of\n"
"         its node.\n"
"\n"
"Returns\n"
"   A dict with the keys path, url, name, title, version, needs_auth,\n"
"   auth_realm, contacts (a list of (name, email)), base_urls, examples (a\n"
"   list of dicts with label and mime), groups and props.  Each prop is a\n"
"   dict with key, section, group, name, title, kind, type, units, value\n"
"   (the default, or None), settable, multi, required, param, enum and\n"
"   range.  The key is what src_url() takes.  multi is None, or for a\n"
"   property that holds a list, (fewest, most) items, most None when\n"
"   unbounded; src_url() takes a list as its items joined by commas.\n";

static PyObject* pyd2_src_info(PyObject* self, PyObject* args)
{
	const char* sPath = NULL;
	if(!PyArg_ParseTuple(args, "s:src_info", &sPath)) return NULL;

	DasSrc* pSrc = _pyd2_newSrc(sPath);
	if(pSrc == NULL) return NULL;
	PyObject* pRet = _pyd2_srcToDict(pSrc);
	del_DasSrc(pSrc);
	return pRet;
}

/* Fill a requisition from a sequence of (key, value) string pairs.  None
 * means nothing to add.  Interface keys only: protocol parameters sent
 * unchecked are a different route, a URL given to read_server(), and the
 * two are not mixed in one request. */
static bool _pyd2_reqLoad(DasReq* pReq, PyObject* pSeq, const char* sWhat)
{
	if((pSeq == NULL)||(pSeq == Py_None)) return true;

	PyObject* pFast = PySequence_Fast(pSeq, "");
	if(pFast == NULL){
		PyErr_Format(PyExc_TypeError, "%s must be a sequence of (key, value) pairs", sWhat);
		return false;
	}
	bool bOk = true;
	Py_ssize_t n = PySequence_Fast_GET_SIZE(pFast);
	for(Py_ssize_t i = 0; bOk && (i < n); ++i){
		PyObject* pPair = PySequence_Fast(PySequence_Fast_GET_ITEM(pFast, i), "");
		const char* sKey = NULL;
		const char* sVal = NULL;
		if((pPair != NULL) && (PySequence_Fast_GET_SIZE(pPair) == 2)){
			PyObject* pK = PySequence_Fast_GET_ITEM(pPair, 0);
			PyObject* pV = PySequence_Fast_GET_ITEM(pPair, 1);
			if(PyUnicode_Check(pK) || PyBytes_Check(pK)) sKey = _pyd2_utf8(pK);
			if(PyUnicode_Check(pV) || PyBytes_Check(pV)) sVal = _pyd2_utf8(pV);
		}
		if((sKey == NULL)||(sVal == NULL)){
			PyErr_Clear();
			PyErr_Format(PyExc_TypeError,
				"%s item %d is not a (key, value) pair of strings", sWhat, (int)i
			);
			bOk = false;
		}
		else{
			if(DasReq_set(pReq, sKey, sVal) != DAS_OKAY){ pyd2_setExceptFromLog(g_pPyD2Error); bOk = false; }
		}
		Py_XDECREF(pPair);
	}
	Py_DECREF(pFast);
	return bOk;
}

/* DasSrc_reqUrl() refused.  The verdicts say why for each bad entry, in
 * text meant to be shown as is; the log has the rest, such as a missing
 * base URL.  Bad entries are the caller's mistake, hence ValueError. */
static void _pyd2_reqRefused(const DasReq* pReq)
{
	char sMsg[D2PY_LOG_BUF_SZ];
	size_t n = (size_t)snprintf(sMsg, sizeof(sMsg), "Request refused:");
	size_t uBad = 0;
	size_t uKeys = DasReq_numKeys(pReq);
	for(size_t u = 0; (u < uKeys) && (n < sizeof(sMsg)); ++u){
		if(DasReq_verdictAt(pReq, u) != DASREQVERDICT_BAD) continue;
		n += snprintf(sMsg + n, sizeof(sMsg) - n, "%s %s", uBad ? ";" : "", DasReq_whyAt(pReq, u));
		++uBad;
	}
	if(uBad == 0){
		pyd2_setExceptFromLog(g_pPyD2Error);
		return;
	}
	/* The refusal was logged too; drop it so a later failure that logs
	   nothing isn't reported with this message */
	memset(g_sLogBuf, 0, D2PY_LOG_BUF_SZ);
	memset(&g_lastErr, 0, sizeof(g_lastErr));
	PyErr_SetString(PyExc_ValueError, sMsg);
}

static char pyd2help_src_url[] =
"Turn a data request into the URL a source's server answers.\n"
"\n"
"Nothing is fetched.  das2C checks every entry against the source and\n"
"translates it to the server's protocol parameters.\n"
"\n"
"Arguments\n"
"   path (string) - The source's catalog path, or the URL or file name of\n"
"         its node.\n"
"\n"
"   entries (sequence, optional) - (key, value) string pairs.  Keys are the\n"
"         'key' of a prop from src_info(), or any shorter spelling das2C\n"
"         accepts, such as 'time.min'.  A coordinate name alone takes a\n"
"         range: ('time', '2026-05-04,2026-05-05') or ('time', '2026-05-04')\n"
"         for that whole day.\n"
"\n"
"   example (int, optional) - Start from this example's settings, entries\n"
"         then adjust them.  -1, the default, starts from nothing.\n"
"\n"
"   base (int, optional) - Which of the source's base_urls to use, 0 unless\n"
"         an earlier one failed to connect.\n"
"\n"
"Returns\n"
"   (url, mime), the request URL and the MIME type expected back.\n"
"\n"
"Raises\n"
"   ValueError if any entry is bad, naming each one and why.\n";

static PyObject* pyd2_src_url(PyObject* self, PyObject* args, PyObject* kwds)
{
	static char* kwlist[] = {"path", "entries", "example", "base", NULL};
	const char* sPath = NULL;
	PyObject* pEntries = NULL;
	int nExample = DASSRC_BLANK_REQ;
	int nBase = 0;

	if(!PyArg_ParseTupleAndKeywords(args, kwds, "s|Oii:src_url", kwlist,
		&sPath, &pEntries, &nExample, &nBase
	))
		return NULL;
	if(nBase < 0){
		PyErr_SetString(PyExc_ValueError, "base must be 0 or more");
		return NULL;
	}

	DasSrc* pSrc = _pyd2_newSrc(sPath);
	if(pSrc == NULL) return NULL;

	PyObject* pRet = NULL;
	char sBuf[4096] = {'\0'};
	char* sUrl = sBuf;
	DasReq* pReq = new_DasReq_src(pSrc, nExample);
	if(pReq == NULL){
		pyd2_setExceptFromLog(PyExc_ValueError);   /* no such example */
		goto DONE;
	}
	if(!_pyd2_reqLoad(pReq, pEntries, "entries")) goto DONE;

	size_t uNeed = DasSrc_reqUrl(pSrc, pReq, (size_t)nBase, sBuf, sizeof(sBuf));
	if(uNeed == 0){
		_pyd2_reqRefused(pReq);
		goto DONE;
	}
	if(uNeed >= sizeof(sBuf)){
		if((sUrl = (char*)calloc(uNeed + 1, 1)) == NULL){ PyErr_NoMemory(); goto DONE; }
		DasSrc_reqUrl(pSrc, pReq, (size_t)nBase, sUrl, uNeed + 1);
	}
	pRet = Py_BuildValue("(ss)", sUrl, DasSrc_mime(pSrc, pReq));

DONE:
	if(sUrl != sBuf) free(sUrl);
	if(pReq != NULL) del_DasReq(pReq);
	del_DasSrc(pSrc);
	return pRet;
}

static char pyd2help_src_loadable[] =
"List the children of a SourceSet or Collection that das2C can load.\n"
"\n"
"These are the sources a program can pick among; which one to use is the\n"
"program's choice, das2C makes none.  The node's own 'catalog' entries\n"
"say what each child provides.\n"
"\n"
"Arguments\n"
"   path (string) - The container's catalog path, or the URL or file name\n"
"         of its node.\n"
"\n"
"Returns\n"
"   A list of child names in node order, empty for a node with none,\n"
"   such as a source itself.\n";

#define D2PY_MAX_KIDS 64

static PyObject* pyd2_src_loadable(PyObject* self, PyObject* args)
{
	const char* sPath = NULL;
	if(!PyArg_ParseTuple(args, "s:src_loadable", &sPath)) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;

	das_catmiss miss;
	DasNode* pNode = DasCat_resolve(g_pCat, sPath, &miss);
	if(pNode == NULL) return _pyd2_setExceptFromMiss(&miss);

	const char* aNames[D2PY_MAX_KIDS];
	size_t uKids = DasSrc_loadable(pNode, aNames, D2PY_MAX_KIDS);
	if(uKids > D2PY_MAX_KIDS) uKids = D2PY_MAX_KIDS;

	PyObject* pList = PyList_New(0);
	if(pList == NULL) return NULL;
	for(size_t u = 0; u < uKids; ++u)
		if(!_pyd2_append(pList, PyString_FromString(aNames[u]))){ Py_DECREF(pList); return NULL; }
	return pList;
}

static char pyd2help_mount[] =
"Put a catalog node, loaded by location, at a name in the catalog.\n"
"\n"
"Paths under the name load from the location and shadow what the das\n"
"catalog says for them; everything else resolves as usual.  Nothing is\n"
"fetched until a path under the name is used.  May be called at any time;\n"
"mounting at a name already mounted replaces the earlier mount.\n"
"\n"
"Arguments\n"
"   url (string) - Where to load the node from, a URL or a file.\n"
"\n"
"   uri (string) - The full catalog name to mount it at, taken as written,\n"
"         such as 'tag:das2.org,2012:site:/uiowa'.  das2C knows no\n"
"         shorthands, 'site:/uiowa' alone names nothing.\n";

static PyObject* pyd2_mount(PyObject* self, PyObject* args)
{
	const char* sUrl = NULL;
	const char* sUri = NULL;
	if(!PyArg_ParseTuple(args, "ss:mount", &sUrl, &sUri)) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;
	if(DasCat_mount(g_pCat, sUrl, sUri) != DAS_OKAY)
		return pyd2_setExceptFromLog(g_pPyD2Error);
	Py_RETURN_NONE;
}

static char pyd2help_seed[] =
"Choose the node catalog paths start from.\n"
"\n"
"Replaces the seed set earlier.  Mounts are kept.  Nothing is fetched until\n"
"a lookup needs the seed.\n"
"\n"
"Arguments\n"
"   url (string, optional) - A URL or file of a node document.  None finds\n"
"         the seed through the das catalog root.\n"
"\n"
"   uri (string, optional) - The full catalog name the node has, taken as\n"
"         written.  None makes the node a root, with no name.\n"
"\n"
"With neither, the seed is the das catalog root, as in a new process.\n";

static PyObject* pyd2_seed(PyObject* self, PyObject* args)
{
	const char* sUrl = NULL;
	const char* sUri = NULL;
	if(!PyArg_ParseTuple(args, "|zz:seed", &sUrl, &sUri)) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;
	if(DasCat_setSeed(g_pCat, sUrl, sUri) != DAS_OKAY)
		return pyd2_setExceptFromLog(g_pPyD2Error);
	Py_RETURN_NONE;
}

static char pyd2help_seed_info[] =
"The seed in effect, as (url, uri).\n"
"\n"
"url is None when the seed comes from the das catalog root that das2C has\n"
"compiled in, which is several mirror URLs rather than one.  uri is '' for\n"
"a seed that is a root.  Loads nothing.\n";

static PyObject* pyd2_seed_info(PyObject* self, PyObject* args)
{
	if(!PyArg_ParseTuple(args, ":seed_info")) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;
	return Py_BuildValue("(NN)",
		_pyd2_strOrNone(DasCat_seedUrl(g_pCat)),
		PyString_FromString(DasCat_seedUri(g_pCat))
	);
}

static char pyd2help_unmount[] =
"Remove a mount, by name or by location.\n"
"\n"
"Arguments\n"
"   url_or_uri (string) - A name given to mount(), matched exactly, or a\n"
"         location, which removes every mount loaded from it.\n"
"\n"
"Raises an error when nothing matched.  The seed is not a mount and is not\n"
"touched.\n";

static PyObject* pyd2_unmount(PyObject* self, PyObject* args)
{
	const char* sWhich = NULL;
	if(!PyArg_ParseTuple(args, "s:unmount", &sWhich)) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;
	if(DasCat_unmount(g_pCat, sWhich) != DAS_OKAY)
		return pyd2_setExceptFromLog(g_pPyD2Error);
	Py_RETURN_NONE;
}

static char pyd2help_mounts[] =
"List the mounts as (url, uri) tuples, in the order they were made.  The\n"
"seed is not a mount, see seed_info().\n";

static PyObject* pyd2_mounts(PyObject* self, PyObject* args)
{
	if(!PyArg_ParseTuple(args, ":mounts")) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;

	PyObject* pList = PyList_New(0);
	if(pList == NULL) return NULL;
	size_t uMounts = DasCat_numMounts(g_pCat);
	for(size_t u = 0; u < uMounts; ++u){
		const char* sUrl = NULL;
		const char* sUri = NULL;
		if(!DasCat_mountAt(g_pCat, u, &sUrl, &sUri)) break;
		if(!_pyd2_append(pList, Py_BuildValue("(ss)", sUrl, sUri))){
			Py_DECREF(pList); return NULL;
		}
	}
	return pList;
}

static char pyd2help_check[] =
"Load the seed and the top node of every mount now.\n"
"\n"
"They are otherwise loaded by the first lookup that needs them, so a wrong\n"
"location is found out late.  Nodes loaded here are kept.\n"
"\n"
"Raises an error naming the first one that would not load, and how many\n"
"failed in all.\n";

static PyObject* pyd2_check(PyObject* self, PyObject* args)
{
	if(!PyArg_ParseTuple(args, ":check")) return NULL;
	if(_pyd2_catalog(NULL) == NULL) return NULL;

	das_catmiss miss;
	int nBad = DasCat_check(g_pCat, &miss);
	if(nBad <= 0) Py_RETURN_NONE;

	memset(g_sLogBuf, 0, D2PY_LOG_BUF_SZ);
	if(nBad == 1)
		PyErr_SetString(g_pPyD2Error, miss.sMsg);
	else
		PyErr_Format(g_pPyD2Error,
			"%d of the seed and mounts did not load, the first: %s", nBad, miss.sMsg
		);
	return NULL;
}
