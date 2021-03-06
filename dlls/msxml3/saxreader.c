/*
 *    SAX Reader implementation
 *
 * Copyright 2008 Alistair Leslie-Hughes
 * Copyright 2008 Piotr Caban
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#define COBJMACROS

#include "config.h"

#include <stdarg.h>
#ifdef HAVE_LIBXML2
# include <libxml/parser.h>
# include <libxml/xmlerror.h>
# include <libxml/SAX2.h>
# include <libxml/parserInternals.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "ole2.h"
#include "msxml6.h"
#include "wininet.h"
#include "urlmon.h"
#include "winreg.h"
#include "shlwapi.h"

#include "wine/debug.h"
#include "wine/list.h"

#include "msxml_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(msxml);

#ifdef HAVE_LIBXML2

typedef enum
{
    ExhaustiveErrors             = 1 << 1,
    ExternalGeneralEntities      = 1 << 2,
    ExternalParameterEntities    = 1 << 3,
    ForcedResync                 = 1 << 4,
    NamespacePrefixes            = 1 << 5,
    Namespaces                   = 1 << 6,
    ParameterEntities            = 1 << 7,
    PreserveSystemIndentifiers   = 1 << 8,
    ProhibitDTD                  = 1 << 9,
    SchemaValidation             = 1 << 10,
    ServerHttpRequest            = 1 << 11,
    SuppressValidationfatalError = 1 << 12,
    UseInlineSchema              = 1 << 13,
    UseSchemaLocation            = 1 << 14,
    LexicalHandlerParEntities    = 1 << 15
} saxreader_features;

struct bstrpool
{
    BSTR *pool;
    unsigned int index;
    unsigned int len;
};

typedef struct
{
    BSTR prefix;
    BSTR uri;
} ns;

typedef struct
{
    struct list entry;
    BSTR prefix;
    BSTR local;
    BSTR qname;
    ns *ns; /* namespaces defined in this particular element */
    int ns_count;
} element_entry;

typedef struct
{
    DispatchEx dispex;
    IVBSAXXMLReader IVBSAXXMLReader_iface;
    ISAXXMLReader ISAXXMLReader_iface;
    LONG ref;
    ISAXContentHandler *contentHandler;
    IVBSAXContentHandler *vbcontentHandler;
    ISAXErrorHandler *errorHandler;
    IVBSAXErrorHandler *vberrorHandler;
    ISAXLexicalHandler *lexicalHandler;
    IVBSAXLexicalHandler *vblexicalHandler;
    ISAXDeclHandler *declHandler;
    IVBSAXDeclHandler *vbdeclHandler;
    xmlSAXHandler sax;
    BOOL isParsing;
    struct bstrpool pool;
    saxreader_features features;
    MSXML_VERSION version;
} saxreader;

typedef struct
{
    IVBSAXLocator IVBSAXLocator_iface;
    ISAXLocator ISAXLocator_iface;
    IVBSAXAttributes IVBSAXAttributes_iface;
    ISAXAttributes ISAXAttributes_iface;
    LONG ref;
    saxreader *saxreader;
    HRESULT ret;
    xmlParserCtxtPtr pParserCtxt;
    WCHAR *publicId;
    WCHAR *systemId;
    int line;
    int column;
    BOOL vbInterface;
    struct list elements;

    BSTR namespaceUri;
    int attributesSize;
    int nb_attributes;
    struct _attributes
    {
        BSTR szLocalname;
        BSTR szURI;
        BSTR szValue;
        BSTR szQName;
    } *attributes;
} saxlocator;

static inline saxreader *impl_from_IVBSAXXMLReader( IVBSAXXMLReader *iface )
{
    return CONTAINING_RECORD(iface, saxreader, IVBSAXXMLReader_iface);
}

static inline saxreader *impl_from_ISAXXMLReader( ISAXXMLReader *iface )
{
    return CONTAINING_RECORD(iface, saxreader, ISAXXMLReader_iface);
}

static inline saxlocator *impl_from_IVBSAXLocator( IVBSAXLocator *iface )
{
    return CONTAINING_RECORD(iface, saxlocator, IVBSAXLocator_iface);
}

static inline saxlocator *impl_from_ISAXLocator( ISAXLocator *iface )
{
    return CONTAINING_RECORD(iface, saxlocator, ISAXLocator_iface);
}

static inline saxlocator *impl_from_IVBSAXAttributes( IVBSAXAttributes *iface )
{
    return CONTAINING_RECORD(iface, saxlocator, IVBSAXAttributes_iface);
}

static inline saxlocator *impl_from_ISAXAttributes( ISAXAttributes *iface )
{
    return CONTAINING_RECORD(iface, saxlocator, ISAXAttributes_iface);
}

/* property names */
static const WCHAR PropertyCharsetW[] = {
    'c','h','a','r','s','e','t',0
};
static const WCHAR PropertyDeclHandlerW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/',
    's','a','x','/','p','r','o','p','e','r','t','i','e','s','/',
    'd','e','c','l','a','r','a','t','i','o','n',
    '-','h','a','n','d','l','e','r',0
};
static const WCHAR PropertyDomNodeW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/',
    's','a','x','/','p','r','o','p','e','r','t','i','e','s','/',
    'd','o','m','-','n','o','d','e',0
};
static const WCHAR PropertyInputSourceW[] = {
    'i','n','p','u','t','-','s','o','u','r','c','e',0
};
static const WCHAR PropertyLexicalHandlerW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/',
    's','a','x','/','p','r','o','p','e','r','t','i','e','s','/',
    'l','e','x','i','c','a','l','-','h','a','n','d','l','e','r',0
};
static const WCHAR PropertyMaxElementDepthW[] = {
    'm','a','x','-','e','l','e','m','e','n','t','-','d','e','p','t','h',0
};
static const WCHAR PropertyMaxXMLSizeW[] = {
    'm','a','x','-','x','m','l','-','s','i','z','e',0
};
static const WCHAR PropertySchemaDeclHandlerW[] = {
    's','c','h','e','m','a','-','d','e','c','l','a','r','a','t','i','o','n','-',
    'h','a','n','d','l','e','r',0
};
static const WCHAR PropertyXMLDeclEncodingW[] = {
    'x','m','l','d','e','c','l','-','e','n','c','o','d','i','n','g',0
};
static const WCHAR PropertyXMLDeclStandaloneW[] = {
    'x','m','l','d','e','c','l','-','s','t','a','n','d','a','l','o','n','e',0
};
static const WCHAR PropertyXMLDeclVersionW[] = {
    'x','m','l','d','e','c','l','-','v','e','r','s','i','o','n',0
};

/* feature names */
static const WCHAR FeatureExternalGeneralEntitiesW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/','s','a','x','/',
    'f','e','a','t','u','r','e','s','/','e','x','t','e','r','n','a','l','-','g','e','n','e','r','a','l',
    '-','e','n','t','i','t','i','e','s',0
};

static const WCHAR FeatureExternalParameterEntitiesW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/','s','a','x','/','f','e','a','t','u','r','e','s',
    '/','e','x','t','e','r','n','a','l','-','p','a','r','a','m','e','t','e','r','-','e','n','t','i','t','i','e','s',0
};

static const WCHAR FeatureLexicalHandlerParEntitiesW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/','s','a','x','/','f','e','a','t','u','r','e','s',
    '/','l','e','x','i','c','a','l','-','h','a','n','d','l','e','r','/','p','a','r','a','m','e','t','e','r','-','e','n','t','i','t','i','e','s',0
};

static const WCHAR FeatureProhibitDTDW[] = {
    'p','r','o','h','i','b','i','t','-','d','t','d',0
};

static const WCHAR FeatureNamespacesW[] = {
    'h','t','t','p',':','/','/','x','m','l','.','o','r','g','/','s','a','x','/','f','e','a','t','u','r','e','s',
    '/','n','a','m','e','s','p','a','c','e','s',0
};

static inline HRESULT set_feature_value(saxreader *reader, saxreader_features feature, VARIANT_BOOL value)
{
    if (value == VARIANT_TRUE)
        reader->features |=  feature;
    else
        reader->features &= ~feature;

    return S_OK;
}

static inline HRESULT get_feature_value(const saxreader *reader, saxreader_features feature, VARIANT_BOOL *value)
{
    *value = reader->features & feature ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static inline BOOL has_content_handler(const saxlocator *locator)
{
    return  (locator->vbInterface && locator->saxreader->vbcontentHandler) ||
           (!locator->vbInterface && locator->saxreader->contentHandler);
}

static inline BOOL has_error_handler(const saxlocator *locator)
{
    return (locator->vbInterface && locator->saxreader->vberrorHandler) ||
          (!locator->vbInterface && locator->saxreader->errorHandler);
}

static BSTR build_qname(BSTR prefix, BSTR local)
{
    if (prefix && *prefix)
    {
        BSTR qname = SysAllocStringLen(NULL, SysStringLen(prefix) + SysStringLen(local) + 1);
        WCHAR *ptr;

        ptr = qname;
        strcpyW(ptr, prefix);
        ptr += SysStringLen(prefix);
        *ptr++ = ':';
        strcpyW(ptr, local);
        return qname;
    }
    else
        return SysAllocString(local);
}

static element_entry* alloc_element_entry(const xmlChar *local, const xmlChar *prefix, int nb_ns,
    const xmlChar **namespaces)
{
    element_entry *ret;
    int i;

    ret = heap_alloc(sizeof(*ret));
    if (!ret) return ret;

    ret->local  = bstr_from_xmlChar(local);
    ret->prefix = bstr_from_xmlChar(prefix);
    ret->qname  = build_qname(ret->prefix, ret->local);
    ret->ns = nb_ns ? heap_alloc(nb_ns*sizeof(ns)) : NULL;
    ret->ns_count = nb_ns;

    for (i=0; i < nb_ns; i++)
    {
        ret->ns[i].prefix = bstr_from_xmlChar(namespaces[2*i]);
        ret->ns[i].uri = bstr_from_xmlChar(namespaces[2*i+1]);
    }

    return ret;
}

static void free_element_entry(element_entry *element)
{
    int i;

    for (i=0; i<element->ns_count;i++)
    {
        SysFreeString(element->ns[i].prefix);
        SysFreeString(element->ns[i].uri);
    }

    SysFreeString(element->prefix);
    SysFreeString(element->local);

    heap_free(element->ns);
    heap_free(element);
}

static void push_element_ns(saxlocator *locator, element_entry *element)
{
    list_add_head(&locator->elements, &element->entry);
}

static element_entry * pop_element_ns(saxlocator *locator)
{
    element_entry *element = LIST_ENTRY(list_head(&locator->elements), element_entry, entry);

    if (element)
        list_remove(&element->entry);

    return element;
}

static BSTR find_element_uri(saxlocator *locator, const xmlChar *uri)
{
    element_entry *element;
    BSTR uriW;
    int i;

    if (!uri) return NULL;

    uriW = bstr_from_xmlChar(uri);

    LIST_FOR_EACH_ENTRY(element, &locator->elements, element_entry, entry)
    {
        for (i=0; i < element->ns_count; i++)
            if (!strcmpW(uriW, element->ns[i].uri))
            {
                SysFreeString(uriW);
                return element->ns[i].uri;
            }
    }

    SysFreeString(uriW);
    ERR("namespace uri not found, %s\n", debugstr_a((char*)uri));
    return NULL;
}

/* used to localize version dependent error check behaviour */
static inline BOOL sax_callback_failed(saxlocator *This, HRESULT hr)
{
    return This->saxreader->version >= MSXML6 ? FAILED(hr) : hr != S_OK;
}

/* index value -1 means it tries to loop for a first time */
static inline BOOL iterate_endprefix_index(saxlocator *This, const element_entry *element, int *i)
{
    if (This->saxreader->version >= MSXML6)
    {
        if (*i == -1) *i = 0; else ++*i;
        return *i < element->ns_count;
    }
    else
    {
        if (*i == -1) *i = element->ns_count-1; else --*i;
        return *i >= 0;
    }
}

static BOOL bstr_pool_insert(struct bstrpool *pool, BSTR pool_entry)
{
    if (!pool->pool)
    {
        pool->pool = HeapAlloc(GetProcessHeap(), 0, 16 * sizeof(*pool->pool));
        if (!pool->pool)
            return FALSE;

        pool->index = 0;
        pool->len = 16;
    }
    else if (pool->index == pool->len)
    {
        BSTR *realloc = HeapReAlloc(GetProcessHeap(), 0, pool->pool, pool->len * 2 * sizeof(*realloc));

        if (!realloc)
            return FALSE;

        pool->pool = realloc;
        pool->len *= 2;
    }

    pool->pool[pool->index++] = pool_entry;
    return TRUE;
}

static void free_bstr_pool(struct bstrpool *pool)
{
    unsigned int i;

    for (i = 0; i < pool->index; i++)
        SysFreeString(pool->pool[i]);

    HeapFree(GetProcessHeap(), 0, pool->pool);

    pool->pool = NULL;
    pool->index = pool->len = 0;
}

static BSTR bstr_from_xmlCharN(const xmlChar *buf, int len)
{
    DWORD dLen;
    BSTR bstr;

    if (!buf)
        return NULL;

    dLen = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)buf, len, NULL, 0);
    if(len != -1) dLen++;
    bstr = SysAllocStringLen(NULL, dLen-1);
    if (!bstr)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)buf, len, bstr, dLen);
    if(len != -1) bstr[dLen-1] = '\0';

    return bstr;
}

static BSTR QName_from_xmlChar(const xmlChar *prefix, const xmlChar *name)
{
    xmlChar *qname;
    BSTR bstr;

    if(!name) return NULL;

    if(!prefix || !*prefix)
        return bstr_from_xmlChar(name);

    qname = xmlBuildQName(name, prefix, NULL, 0);
    bstr = bstr_from_xmlChar(qname);
    xmlFree(qname);

    return bstr;
}

static BSTR pooled_bstr_from_xmlChar(struct bstrpool *pool, const xmlChar *buf)
{
    BSTR pool_entry = bstr_from_xmlChar(buf);

    if (pool_entry && !bstr_pool_insert(pool, pool_entry))
    {
        SysFreeString(pool_entry);
        return NULL;
    }

    return pool_entry;
}

static BSTR pooled_bstr_from_xmlCharN(struct bstrpool *pool, const xmlChar *buf, int len)
{
    BSTR pool_entry = bstr_from_xmlCharN(buf, len);

    if (pool_entry && !bstr_pool_insert(pool, pool_entry))
    {
        SysFreeString(pool_entry);
        return NULL;
    }

    return pool_entry;
}

static void format_error_message_from_id(saxlocator *This, HRESULT hr)
{
    xmlStopParser(This->pParserCtxt);
    This->ret = hr;

    if(has_error_handler(This))
    {
        WCHAR msg[1024];
        if(!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM,
                    NULL, hr, 0, msg, sizeof(msg), NULL))
        {
            FIXME("MSXML errors not yet supported.\n");
            msg[0] = '\0';
        }

        if(This->vbInterface)
        {
            BSTR bstrMsg = SysAllocString(msg);
            IVBSAXErrorHandler_fatalError(This->saxreader->vberrorHandler,
                    &This->IVBSAXLocator_iface, &bstrMsg, hr);
            SysFreeString(bstrMsg);
        }
        else
            ISAXErrorHandler_fatalError(This->saxreader->errorHandler,
                    &This->ISAXLocator_iface, msg, hr);
    }
}

static void update_position(saxlocator *This, BOOL fix_column)
{
    const xmlChar *p = This->pParserCtxt->input->cur-1;

    This->line = xmlSAX2GetLineNumber(This->pParserCtxt);
    if(fix_column)
    {
        This->column = 1;
        for(; *p!='\n' && *p!='\r' && p>=This->pParserCtxt->input->base; p--)
            This->column++;
    }
    else
    {
        This->column = xmlSAX2GetColumnNumber(This->pParserCtxt);
    }
}

/*** IVBSAXAttributes interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI ivbsaxattributes_QueryInterface(
        IVBSAXAttributes* iface,
        REFIID riid,
        void **ppvObject)
{
    saxlocator *This = impl_from_IVBSAXAttributes(iface);
    TRACE("%p %s %p\n", This, debugstr_guid(riid), ppvObject);
    return IVBSAXLocator_QueryInterface(&This->IVBSAXLocator_iface, riid, ppvObject);
}

static ULONG WINAPI ivbsaxattributes_AddRef(IVBSAXAttributes* iface)
{
    saxlocator *This = impl_from_IVBSAXAttributes(iface);
    return ISAXLocator_AddRef(&This->ISAXLocator_iface);
}

static ULONG WINAPI ivbsaxattributes_Release(IVBSAXAttributes* iface)
{
    saxlocator *This = impl_from_IVBSAXAttributes(iface);
    return ISAXLocator_Release(&This->ISAXLocator_iface);
}

/*** IDispatch methods ***/
static HRESULT WINAPI ivbsaxattributes_GetTypeInfoCount( IVBSAXAttributes *iface, UINT* pctinfo )
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );

    TRACE("(%p)->(%p)\n", This, pctinfo);

    *pctinfo = 1;

    return S_OK;
}

static HRESULT WINAPI ivbsaxattributes_GetTypeInfo(
    IVBSAXAttributes *iface,
    UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo )
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    HRESULT hr;

    TRACE("(%p)->(%u %u %p)\n", This, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(IVBSAXAttributes_tid, ppTInfo);

    return hr;
}

static HRESULT WINAPI ivbsaxattributes_GetIDsOfNames(
    IVBSAXAttributes *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId)
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%s %p %u %u %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    if(!rgszNames || cNames == 0 || !rgDispId)
        return E_INVALIDARG;

    hr = get_typeinfo(IVBSAXAttributes_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames(typeinfo, rgszNames, cNames, rgDispId);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

static HRESULT WINAPI ivbsaxattributes_Invoke(
    IVBSAXAttributes *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr)
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%d %s %d %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(IVBSAXAttributes_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke(typeinfo, &This->IVBSAXAttributes_iface, dispIdMember, wFlags,
                pDispParams, pVarResult, pExcepInfo, puArgErr);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

/*** IVBSAXAttributes methods ***/
static HRESULT WINAPI ivbsaxattributes_get_length(
        IVBSAXAttributes* iface,
        int *nLength)
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getLength(&This->ISAXAttributes_iface, nLength);
}

static HRESULT WINAPI ivbsaxattributes_getURI(
        IVBSAXAttributes* iface,
        int nIndex,
        BSTR *uri)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getURI(&This->ISAXAttributes_iface, nIndex, (const WCHAR**)uri, &len);
}

static HRESULT WINAPI ivbsaxattributes_getLocalName(
        IVBSAXAttributes* iface,
        int nIndex,
        BSTR *localName)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getLocalName(&This->ISAXAttributes_iface, nIndex,
            (const WCHAR**)localName, &len);
}

static HRESULT WINAPI ivbsaxattributes_getQName(
        IVBSAXAttributes* iface,
        int nIndex,
        BSTR *QName)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getQName(&This->ISAXAttributes_iface, nIndex, (const WCHAR**)QName, &len);
}

static HRESULT WINAPI ivbsaxattributes_getIndexFromName(
        IVBSAXAttributes* iface,
        BSTR uri,
        BSTR localName,
        int *index)
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getIndexFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            localName, SysStringLen(localName), index);
}

static HRESULT WINAPI ivbsaxattributes_getIndexFromQName(
        IVBSAXAttributes* iface,
        BSTR QName,
        int *index)
{
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getIndexFromQName(&This->ISAXAttributes_iface, QName,
            SysStringLen(QName), index);
}

static HRESULT WINAPI ivbsaxattributes_getType(
        IVBSAXAttributes* iface,
        int nIndex,
        BSTR *type)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getType(&This->ISAXAttributes_iface, nIndex, (const WCHAR**)type, &len);
}

static HRESULT WINAPI ivbsaxattributes_getTypeFromName(
        IVBSAXAttributes* iface,
        BSTR uri,
        BSTR localName,
        BSTR *type)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getTypeFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            localName, SysStringLen(localName), (const WCHAR**)type, &len);
}

static HRESULT WINAPI ivbsaxattributes_getTypeFromQName(
        IVBSAXAttributes* iface,
        BSTR QName,
        BSTR *type)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getTypeFromQName(&This->ISAXAttributes_iface, QName, SysStringLen(QName),
            (const WCHAR**)type, &len);
}

static HRESULT WINAPI ivbsaxattributes_getValue(
        IVBSAXAttributes* iface,
        int nIndex,
        BSTR *value)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getValue(&This->ISAXAttributes_iface, nIndex, (const WCHAR**)value, &len);
}

static HRESULT WINAPI ivbsaxattributes_getValueFromName(
        IVBSAXAttributes* iface,
        BSTR uri,
        BSTR localName,
        BSTR *value)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getValueFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            localName, SysStringLen(localName), (const WCHAR**)value, &len);
}

static HRESULT WINAPI ivbsaxattributes_getValueFromQName(
        IVBSAXAttributes* iface,
        BSTR QName,
        BSTR *value)
{
    int len;
    saxlocator *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getValueFromQName(&This->ISAXAttributes_iface, QName,
            SysStringLen(QName), (const WCHAR**)value, &len);
}

static const struct IVBSAXAttributesVtbl ivbsaxattributes_vtbl =
{
    ivbsaxattributes_QueryInterface,
    ivbsaxattributes_AddRef,
    ivbsaxattributes_Release,
    ivbsaxattributes_GetTypeInfoCount,
    ivbsaxattributes_GetTypeInfo,
    ivbsaxattributes_GetIDsOfNames,
    ivbsaxattributes_Invoke,
    ivbsaxattributes_get_length,
    ivbsaxattributes_getURI,
    ivbsaxattributes_getLocalName,
    ivbsaxattributes_getQName,
    ivbsaxattributes_getIndexFromName,
    ivbsaxattributes_getIndexFromQName,
    ivbsaxattributes_getType,
    ivbsaxattributes_getTypeFromName,
    ivbsaxattributes_getTypeFromQName,
    ivbsaxattributes_getValue,
    ivbsaxattributes_getValueFromName,
    ivbsaxattributes_getValueFromQName
};

/*** ISAXAttributes interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI isaxattributes_QueryInterface(
        ISAXAttributes* iface,
        REFIID riid,
        void **ppvObject)
{
    saxlocator *This = impl_from_ISAXAttributes(iface);
    TRACE("%p %s %p\n", This, debugstr_guid(riid), ppvObject);
    return ISAXLocator_QueryInterface(&This->ISAXLocator_iface, riid, ppvObject);
}

static ULONG WINAPI isaxattributes_AddRef(ISAXAttributes* iface)
{
    saxlocator *This = impl_from_ISAXAttributes(iface);
    TRACE("%p\n", This);
    return ISAXLocator_AddRef(&This->ISAXLocator_iface);
}

static ULONG WINAPI isaxattributes_Release(ISAXAttributes* iface)
{
    saxlocator *This = impl_from_ISAXAttributes(iface);

    TRACE("%p\n", This);
    return ISAXLocator_Release(&This->ISAXLocator_iface);
}

/*** ISAXAttributes methods ***/
static HRESULT WINAPI isaxattributes_getLength(
        ISAXAttributes* iface,
        int *length)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );

    *length = This->nb_attributes;
    TRACE("Length set to %d\n", *length);
    return S_OK;
}

static HRESULT WINAPI isaxattributes_getURI(
        ISAXAttributes* iface,
        int index,
        const WCHAR **url,
        int *size)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%d)\n", This, index);

    if(index >= This->nb_attributes || index < 0) return E_INVALIDARG;
    if(!url || !size) return E_POINTER;

    *size = SysStringLen(This->attributes[index].szURI);
    *url = This->attributes[index].szURI;

    TRACE("(%s:%d)\n", debugstr_w(This->attributes[index].szURI), *size);

    return S_OK;
}

static HRESULT WINAPI isaxattributes_getLocalName(
        ISAXAttributes* iface,
        int nIndex,
        const WCHAR **pLocalName,
        int *pLocalNameLength)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%d)\n", This, nIndex);

    if(nIndex>=This->nb_attributes || nIndex<0) return E_INVALIDARG;
    if(!pLocalName || !pLocalNameLength) return E_POINTER;

    *pLocalNameLength = SysStringLen(This->attributes[nIndex].szLocalname);
    *pLocalName = This->attributes[nIndex].szLocalname;

    return S_OK;
}

static HRESULT WINAPI isaxattributes_getQName(
        ISAXAttributes* iface,
        int nIndex,
        const WCHAR **pQName,
        int *pQNameLength)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%d)\n", This, nIndex);

    if(nIndex>=This->nb_attributes || nIndex<0) return E_INVALIDARG;
    if(!pQName || !pQNameLength) return E_POINTER;

    *pQNameLength = SysStringLen(This->attributes[nIndex].szQName);
    *pQName = This->attributes[nIndex].szQName;

    return S_OK;
}

static HRESULT WINAPI isaxattributes_getName(
        ISAXAttributes* iface,
        int index,
        const WCHAR **uri,
        int *pUriLength,
        const WCHAR **localName,
        int *pLocalNameSize,
        const WCHAR **QName,
        int *pQNameLength)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%d)\n", This, index);

    if(index>=This->nb_attributes || index<0) return E_INVALIDARG;
    if(!uri || !pUriLength || !localName || !pLocalNameSize
            || !QName || !pQNameLength) return E_POINTER;

    *pUriLength = SysStringLen(This->attributes[index].szURI);
    *uri = This->attributes[index].szURI;
    *pLocalNameSize = SysStringLen(This->attributes[index].szLocalname);
    *localName = This->attributes[index].szLocalname;
    *pQNameLength = SysStringLen(This->attributes[index].szQName);
    *QName = This->attributes[index].szQName;

    TRACE("(%s, %s, %s)\n", debugstr_w(*uri), debugstr_w(*localName), debugstr_w(*QName));

    return S_OK;
}

static HRESULT WINAPI isaxattributes_getIndexFromName(
        ISAXAttributes* iface,
        const WCHAR *pUri,
        int cUriLength,
        const WCHAR *pLocalName,
        int cocalNameLength,
        int *index)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    int i;
    TRACE("(%p)->(%s, %d, %s, %d)\n", This, debugstr_w(pUri), cUriLength,
            debugstr_w(pLocalName), cocalNameLength);

    if(!pUri || !pLocalName || !index) return E_POINTER;

    for(i=0; i<This->nb_attributes; i++)
    {
        if(cUriLength!=SysStringLen(This->attributes[i].szURI)
                || cocalNameLength!=SysStringLen(This->attributes[i].szLocalname))
            continue;
        if(cUriLength && memcmp(pUri, This->attributes[i].szURI,
                    sizeof(WCHAR)*cUriLength))
            continue;
        if(cocalNameLength && memcmp(pLocalName, This->attributes[i].szLocalname,
                    sizeof(WCHAR)*cocalNameLength))
            continue;

        *index = i;
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI isaxattributes_getIndexFromQName(
        ISAXAttributes* iface,
        const WCHAR *pQName,
        int nQNameLength,
        int *index)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    int i;
    TRACE("(%p)->(%s, %d)\n", This, debugstr_w(pQName), nQNameLength);

    if(!pQName || !index) return E_POINTER;
    if(!nQNameLength) return E_INVALIDARG;

    for(i=0; i<This->nb_attributes; i++)
    {
        if(nQNameLength!=SysStringLen(This->attributes[i].szQName)) continue;
        if(memcmp(pQName, This->attributes[i].szQName, sizeof(WCHAR)*nQNameLength)) continue;

        *index = i;
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI isaxattributes_getType(
        ISAXAttributes* iface,
        int nIndex,
        const WCHAR **pType,
        int *pTypeLength)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );

    FIXME("(%p)->(%d) stub\n", This, nIndex);
    return E_NOTIMPL;
}

static HRESULT WINAPI isaxattributes_getTypeFromName(
        ISAXAttributes* iface,
        const WCHAR *pUri,
        int nUri,
        const WCHAR *pLocalName,
        int nLocalName,
        const WCHAR **pType,
        int *nType)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );

    FIXME("(%p)->(%s, %d, %s, %d) stub\n", This, debugstr_w(pUri), nUri,
            debugstr_w(pLocalName), nLocalName);
    return E_NOTIMPL;
}

static HRESULT WINAPI isaxattributes_getTypeFromQName(
        ISAXAttributes* iface,
        const WCHAR *pQName,
        int nQName,
        const WCHAR **pType,
        int *nType)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );

    FIXME("(%p)->(%s, %d) stub\n", This, debugstr_w(pQName), nQName);
    return E_NOTIMPL;
}

static HRESULT WINAPI isaxattributes_getValue(
        ISAXAttributes* iface,
        int index,
        const WCHAR **value,
        int *nValue)
{
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%d)\n", This, index);

    if(index>=This->nb_attributes || index<0) return E_INVALIDARG;
    if(!value || !nValue) return E_POINTER;

    *nValue = SysStringLen(This->attributes[index].szValue);
    *value = This->attributes[index].szValue;

    TRACE("(%s:%d)\n", debugstr_w(*value), *nValue);

    return S_OK;
}

static HRESULT WINAPI isaxattributes_getValueFromName(
        ISAXAttributes* iface,
        const WCHAR *pUri,
        int nUri,
        const WCHAR *pLocalName,
        int nLocalName,
        const WCHAR **pValue,
        int *nValue)
{
    HRESULT hr;
    int index;
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%s, %d, %s, %d)\n", This, debugstr_w(pUri), nUri,
            debugstr_w(pLocalName), nLocalName);

    hr = ISAXAttributes_getIndexFromName(iface,
            pUri, nUri, pLocalName, nLocalName, &index);
    if(hr==S_OK) hr = ISAXAttributes_getValue(iface, index, pValue, nValue);

    return hr;
}

static HRESULT WINAPI isaxattributes_getValueFromQName(
        ISAXAttributes* iface,
        const WCHAR *pQName,
        int nQName,
        const WCHAR **pValue,
        int *nValue)
{
    HRESULT hr;
    int index;
    saxlocator *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%s, %d)\n", This, debugstr_w(pQName), nQName);

    hr = ISAXAttributes_getIndexFromQName(iface, pQName, nQName, &index);
    if(hr==S_OK) hr = ISAXAttributes_getValue(iface, index, pValue, nValue);

    return hr;
}

static const struct ISAXAttributesVtbl isaxattributes_vtbl =
{
    isaxattributes_QueryInterface,
    isaxattributes_AddRef,
    isaxattributes_Release,
    isaxattributes_getLength,
    isaxattributes_getURI,
    isaxattributes_getLocalName,
    isaxattributes_getQName,
    isaxattributes_getName,
    isaxattributes_getIndexFromName,
    isaxattributes_getIndexFromQName,
    isaxattributes_getType,
    isaxattributes_getTypeFromName,
    isaxattributes_getTypeFromQName,
    isaxattributes_getValue,
    isaxattributes_getValueFromName,
    isaxattributes_getValueFromQName
};

static HRESULT SAXAttributes_populate(saxlocator *locator,
        int nb_namespaces, const xmlChar **xmlNamespaces,
        int nb_attributes, const xmlChar **xmlAttributes)
{
    static const xmlChar xmlns[] = "xmlns";
    static const WCHAR xmlnsW[] = { 'x','m','l','n','s',0 };

    struct _attributes *attrs;
    int index;

    locator->nb_attributes = nb_namespaces+nb_attributes;
    if(locator->nb_attributes > locator->attributesSize)
    {
        attrs = heap_realloc(locator->attributes, sizeof(struct _attributes)*locator->nb_attributes*2);
        if(!attrs)
        {
            locator->nb_attributes = 0;
            return E_OUTOFMEMORY;
        }
        locator->attributes = attrs;
    }
    else
    {
        attrs = locator->attributes;
    }

    for(index=0; index<nb_namespaces; index++)
    {
        attrs[nb_attributes+index].szLocalname = SysAllocStringLen(NULL, 0);
        attrs[nb_attributes+index].szURI = locator->namespaceUri;
        attrs[nb_attributes+index].szValue = bstr_from_xmlChar(xmlNamespaces[2*index+1]);
        if(!xmlNamespaces[2*index])
            attrs[nb_attributes+index].szQName = SysAllocString(xmlnsW);
        else
            attrs[nb_attributes+index].szQName = QName_from_xmlChar(xmlns, xmlNamespaces[2*index]);
    }

    for(index=0; index<nb_attributes; index++)
    {
        attrs[index].szLocalname = bstr_from_xmlChar(xmlAttributes[index*5]);
        attrs[index].szURI = find_element_uri(locator, xmlAttributes[index*5+2]);
        attrs[index].szValue = bstr_from_xmlCharN(xmlAttributes[index*5+3],
                xmlAttributes[index*5+4]-xmlAttributes[index*5+3]);
        attrs[index].szQName = QName_from_xmlChar(xmlAttributes[index*5+1],
                xmlAttributes[index*5]);
    }

    return S_OK;
}

/*** LibXML callbacks ***/
static void libxmlStartDocument(void *ctx)
{
    saxlocator *This = ctx;
    HRESULT hr;

    if(This->saxreader->version >= MSXML6)
    {
        const xmlChar *p = This->pParserCtxt->input->cur-1;
        update_position(This, FALSE);
        while(p>This->pParserCtxt->input->base && *p!='>')
        {
            if(*p=='\n' || (*p=='\r' && *(p+1)!='\n'))
                This->line--;
            p--;
        }
        This->column = 0;
        for(; p>=This->pParserCtxt->input->base && *p!='\n' && *p!='\r'; p--)
            This->column++;
    }

    if(has_content_handler(This))
    {
        if(This->vbInterface)
            hr = IVBSAXContentHandler_startDocument(This->saxreader->vbcontentHandler);
        else
            hr = ISAXContentHandler_startDocument(This->saxreader->contentHandler);

        if (sax_callback_failed(This, hr))
            format_error_message_from_id(This, hr);
    }
}

static void libxmlEndDocument(void *ctx)
{
    saxlocator *This = ctx;
    HRESULT hr;

    if(This->saxreader->version >= MSXML6) {
        update_position(This, FALSE);
        if(This->column > 1)
            This->line++;
        This->column = 0;
    } else {
        This->column = 0;
        This->line = 0;
    }

    if(This->ret != S_OK) return;

    if(has_content_handler(This))
    {
        if(This->vbInterface)
            hr = IVBSAXContentHandler_endDocument(This->saxreader->vbcontentHandler);
        else
            hr = ISAXContentHandler_endDocument(This->saxreader->contentHandler);

        if (sax_callback_failed(This, hr))
            format_error_message_from_id(This, hr);
    }
}

static void libxmlStartElementNS(
        void *ctx,
        const xmlChar *localname,
        const xmlChar *prefix,
        const xmlChar *URI,
        int nb_namespaces,
        const xmlChar **namespaces,
        int nb_attributes,
        int nb_defaulted,
        const xmlChar **attributes)
{
    saxlocator *This = ctx;
    element_entry *element;
    HRESULT hr = S_OK;

    update_position(This, TRUE);
    if(*(This->pParserCtxt->input->cur) == '/')
        This->column++;
    if(This->saxreader->version < MSXML6)
        This->column++;

    element = alloc_element_entry(localname, prefix, nb_namespaces, namespaces);
    push_element_ns(This, element);

    if (has_content_handler(This))
    {
        BSTR uri;
        int i;

        for (i = 0; i < nb_namespaces; i++)
        {
            if(This->vbInterface)
                hr = IVBSAXContentHandler_startPrefixMapping(
                        This->saxreader->vbcontentHandler,
                        &element->ns[i].prefix,
                        &element->ns[i].uri);
            else
                hr = ISAXContentHandler_startPrefixMapping(
                        This->saxreader->contentHandler,
                        element->ns[i].prefix,
                        SysStringLen(element->ns[i].prefix),
                        element->ns[i].uri,
                        SysStringLen(element->ns[i].uri));

            if (sax_callback_failed(This, hr))
            {
                format_error_message_from_id(This, hr);
                return;
            }
        }

        uri = find_element_uri(This, URI);

        hr = SAXAttributes_populate(This, nb_namespaces, namespaces, nb_attributes, attributes);
        if(hr == S_OK)
        {
            if(This->vbInterface)
                hr = IVBSAXContentHandler_startElement(This->saxreader->vbcontentHandler,
                        &uri, &element->local, &element->qname, &This->IVBSAXAttributes_iface);
            else
                hr = ISAXContentHandler_startElement(This->saxreader->contentHandler,
                        uri, SysStringLen(uri),
                        element->local, SysStringLen(element->local),
                        element->qname, SysStringLen(element->qname),
                        &This->ISAXAttributes_iface);
        }
    }

    if (sax_callback_failed(This, hr))
        format_error_message_from_id(This, hr);
}

static void libxmlEndElementNS(
        void *ctx,
        const xmlChar *localname,
        const xmlChar *prefix,
        const xmlChar *URI)
{
    saxlocator *This = ctx;
    element_entry *element;
    const xmlChar *p;
    HRESULT hr;
    BSTR uri;
    int i;

    update_position(This, FALSE);
    p = This->pParserCtxt->input->cur;
    if(This->saxreader->version >= MSXML6)
    {
        p--;
        while(p>This->pParserCtxt->input->base && *p!='>')
        {
            if(*p=='\n' || (*p=='\r' && *(p+1)!='\n'))
                This->line--;
            p--;
        }
    }
    else if(*(p-1)!='>' || *(p-2)!='/')
    {
        p--;
        while(p-2>=This->pParserCtxt->input->base
                && *(p-2)!='<' && *(p-1)!='/')
        {
            if(*p=='\n' || (*p=='\r' && *(p+1)!='\n'))
                This->line--;
            p--;
        }
    }
    This->column = 0;
    for(; p>=This->pParserCtxt->input->base && *p!='\n' && *p!='\r'; p--)
        This->column++;

    uri = find_element_uri(This, URI);
    element = pop_element_ns(This);

    if (!has_content_handler(This))
    {
        This->nb_attributes = 0;
        free_element_entry(element);
        return;
    }

    if(This->vbInterface)
        hr = IVBSAXContentHandler_endElement(
                This->saxreader->vbcontentHandler,
                &uri, &element->local, &element->qname);
    else
        hr = ISAXContentHandler_endElement(
                This->saxreader->contentHandler,
                uri, SysStringLen(uri),
                element->local, SysStringLen(element->local),
                element->qname, SysStringLen(element->qname));

    This->nb_attributes = 0;

    if (sax_callback_failed(This, hr))
    {
        format_error_message_from_id(This, hr);
        return;
    }

    i = -1;
    while (iterate_endprefix_index(This, element, &i))
    {
        if(This->vbInterface)
            hr = IVBSAXContentHandler_endPrefixMapping(
                    This->saxreader->vbcontentHandler, &element->ns[i].prefix);
        else
            hr = ISAXContentHandler_endPrefixMapping(
                    This->saxreader->contentHandler,
                    element->ns[i].prefix, SysStringLen(element->ns[i].prefix));

        if (sax_callback_failed(This, hr)) break;
    }

    if (sax_callback_failed(This, hr))
        format_error_message_from_id(This, hr);

    free_element_entry(element);
}

static void libxmlCharacters(
        void *ctx,
        const xmlChar *ch,
        int len)
{
    saxlocator *This = ctx;
    BSTR Chars;
    HRESULT hr;
    xmlChar *cur, *end;
    BOOL lastEvent = FALSE;

    if(!(has_content_handler(This))) return;

    update_position(This, FALSE);
    cur = (xmlChar*)This->pParserCtxt->input->cur;
    while(cur>=This->pParserCtxt->input->base && *cur!='>')
    {
        if(*cur=='\n' || (*cur=='\r' && *(cur+1)!='\n'))
            This->line--;
        cur--;
    }
    This->column = 1;
    for(; cur>=This->pParserCtxt->input->base && *cur!='\n' && *cur!='\r'; cur--)
        This->column++;

    cur = (xmlChar*)ch;
    if(*(ch-1)=='\r') cur--;
    end = cur;

    while(1)
    {
        while(end-ch<len && *end!='\r') end++;
        if(end-ch==len)
        {
            lastEvent = TRUE;
        }
        else
        {
            *end = '\n';
            end++;
        }

        if(This->saxreader->version >= MSXML6)
        {
            xmlChar *p;

            for(p=cur; p!=end; p++)
            {
                if(*p=='\n')
                {
                    This->line++;
                    This->column = 1;
                }
                else
                {
                    This->column++;
                }
            }

            if(!lastEvent)
                This->column = 0;
        }

        Chars = pooled_bstr_from_xmlCharN(&This->saxreader->pool, cur, end-cur);
        if(This->vbInterface)
            hr = IVBSAXContentHandler_characters(
                    This->saxreader->vbcontentHandler, &Chars);
        else
            hr = ISAXContentHandler_characters(
                    This->saxreader->contentHandler,
                    Chars, SysStringLen(Chars));

        if (sax_callback_failed(This, hr))
        {
            format_error_message_from_id(This, hr);
            return;
        }

        if(This->saxreader->version < MSXML6)
            This->column += end-cur;

        if(lastEvent)
            break;

        *(end-1) = '\r';
        if(*end == '\n')
        {
            end++;
            This->column++;
        }
        cur = end;

        if(end-ch == len) break;
    }
}

static void libxmlSetDocumentLocator(
        void *ctx,
        xmlSAXLocatorPtr loc)
{
    saxlocator *This = ctx;
    HRESULT hr = S_OK;

    if(has_content_handler(This))
    {
        if(This->vbInterface)
            hr = IVBSAXContentHandler_putref_documentLocator(This->saxreader->vbcontentHandler,
                    &This->IVBSAXLocator_iface);
        else
            hr = ISAXContentHandler_putDocumentLocator(This->saxreader->contentHandler,
                    &This->ISAXLocator_iface);
    }

    if(FAILED(hr))
        format_error_message_from_id(This, hr);
}

static void libxmlComment(void *ctx, const xmlChar *value)
{
    saxlocator *This = ctx;
    BSTR bValue;
    HRESULT hr;
    const xmlChar *p = This->pParserCtxt->input->cur;

    update_position(This, FALSE);
    while(p-4>=This->pParserCtxt->input->base
            && memcmp(p-4, "<!--", sizeof(char[4])))
    {
        if(*p=='\n' || (*p=='\r' && *(p+1)!='\n'))
            This->line--;
        p--;
    }
    This->column = 0;
    for(; p>=This->pParserCtxt->input->base && *p!='\n' && *p!='\r'; p--)
        This->column++;

    if(!This->vbInterface && !This->saxreader->lexicalHandler) return;
    if(This->vbInterface && !This->saxreader->vblexicalHandler) return;

    bValue = pooled_bstr_from_xmlChar(&This->saxreader->pool, value);

    if(This->vbInterface)
        hr = IVBSAXLexicalHandler_comment(
                This->saxreader->vblexicalHandler, &bValue);
    else
        hr = ISAXLexicalHandler_comment(
                This->saxreader->lexicalHandler,
                bValue, SysStringLen(bValue));

    if(FAILED(hr))
        format_error_message_from_id(This, hr);
}

static void libxmlFatalError(void *ctx, const char *msg, ...)
{
    saxlocator *This = ctx;
    char message[1024];
    WCHAR *error;
    DWORD len;
    va_list args;

    if(This->ret != S_OK) {
        xmlStopParser(This->pParserCtxt);
        return;
    }

    va_start(args, msg);
    vsprintf(message, msg, args);
    va_end(args);

    len = MultiByteToWideChar(CP_UNIXCP, 0, message, -1, NULL, 0);
    error = heap_alloc(sizeof(WCHAR)*len);
    if(error)
    {
        MultiByteToWideChar(CP_UNIXCP, 0, message, -1, error, len);
        TRACE("fatal error for %p: %s\n", This, debugstr_w(error));
    }

    if(!has_error_handler(This))
    {
        xmlStopParser(This->pParserCtxt);
        This->ret = E_FAIL;
        heap_free(error);
        return;
    }

    FIXME("Error handling is not compatible.\n");

    if(This->vbInterface)
    {
        BSTR bstrError = SysAllocString(error);
        IVBSAXErrorHandler_fatalError(This->saxreader->vberrorHandler, &This->IVBSAXLocator_iface,
                &bstrError, E_FAIL);
        SysFreeString(bstrError);
    }
    else
        ISAXErrorHandler_fatalError(This->saxreader->errorHandler, &This->ISAXLocator_iface,
                error, E_FAIL);

    heap_free(error);

    xmlStopParser(This->pParserCtxt);
    This->ret = E_FAIL;
}

static void libxmlCDataBlock(void *ctx, const xmlChar *value, int len)
{
    saxlocator *This = ctx;
    HRESULT hr = S_OK;
    xmlChar *beg = (xmlChar*)This->pParserCtxt->input->cur-len;
    xmlChar *cur, *end;
    int realLen;
    BSTR Chars;
    BOOL lastEvent = FALSE, change;

    update_position(This, FALSE);
    while(beg-9>=This->pParserCtxt->input->base
            && memcmp(beg-9, "<![CDATA[", sizeof(char[9])))
    {
        if(*beg=='\n' || (*beg=='\r' && *(beg+1)!='\n'))
            This->line--;
        beg--;
    }
    This->column = 0;
    for(; beg>=This->pParserCtxt->input->base && *beg!='\n' && *beg!='\r'; beg--)
        This->column++;

    if(This->vbInterface && This->saxreader->vblexicalHandler)
        hr = IVBSAXLexicalHandler_startCDATA(This->saxreader->vblexicalHandler);
    if(!This->vbInterface && This->saxreader->lexicalHandler)
        hr = ISAXLexicalHandler_startCDATA(This->saxreader->lexicalHandler);

    if(FAILED(hr))
    {
        format_error_message_from_id(This, hr);
        return;
    }

    realLen = This->pParserCtxt->input->cur-beg-3;
    cur = beg;
    end = beg;

    while(1)
    {
        while(end-beg<realLen && *end!='\r') end++;
        if(end-beg==realLen)
        {
            end--;
            lastEvent = TRUE;
        }
        else if(end-beg==realLen-1 && *end=='\r' && *(end+1)=='\n')
            lastEvent = TRUE;

        if(*end == '\r') change = TRUE;
        else change = FALSE;

        if(change) *end = '\n';

        if(has_content_handler(This))
        {
            Chars = pooled_bstr_from_xmlCharN(&This->saxreader->pool, cur, end-cur+1);
            if(This->vbInterface)
                hr = IVBSAXContentHandler_characters(
                        This->saxreader->vbcontentHandler, &Chars);
            else
                hr = ISAXContentHandler_characters(
                        This->saxreader->contentHandler,
                        Chars, SysStringLen(Chars));
        }

        if(change) *end = '\r';

        if(lastEvent)
            break;

        This->column += end-cur+2;
        end += 2;
        cur = end;
    }

    if(This->vbInterface && This->saxreader->vblexicalHandler)
        hr = IVBSAXLexicalHandler_endCDATA(This->saxreader->vblexicalHandler);
    if(!This->vbInterface && This->saxreader->lexicalHandler)
        hr = ISAXLexicalHandler_endCDATA(This->saxreader->lexicalHandler);

    if(FAILED(hr))
        format_error_message_from_id(This, hr);

    This->column += 4+end-cur;
}

/*** IVBSAXLocator interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI ivbsaxlocator_QueryInterface(IVBSAXLocator* iface, REFIID riid, void **ppvObject)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), ppvObject);

    *ppvObject = NULL;

    if ( IsEqualGUID( riid, &IID_IUnknown ) ||
            IsEqualGUID( riid, &IID_IDispatch) ||
            IsEqualGUID( riid, &IID_IVBSAXLocator ))
    {
        *ppvObject = iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXAttributes ))
    {
        *ppvObject = &This->IVBSAXAttributes_iface;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    IVBSAXLocator_AddRef( iface );

    return S_OK;
}

static ULONG WINAPI ivbsaxlocator_AddRef(IVBSAXLocator* iface)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    TRACE("%p\n", This );
    return InterlockedIncrement( &This->ref );
}

static ULONG WINAPI ivbsaxlocator_Release(
        IVBSAXLocator* iface)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    return ISAXLocator_Release((ISAXLocator*)&This->IVBSAXLocator_iface);
}

/*** IDispatch methods ***/
static HRESULT WINAPI ivbsaxlocator_GetTypeInfoCount( IVBSAXLocator *iface, UINT* pctinfo )
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );

    TRACE("(%p)->(%p)\n", This, pctinfo);

    *pctinfo = 1;

    return S_OK;
}

static HRESULT WINAPI ivbsaxlocator_GetTypeInfo(
    IVBSAXLocator *iface,
    UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo )
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    HRESULT hr;

    TRACE("(%p)->(%u %u %p)\n", This, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(IVBSAXLocator_tid, ppTInfo);

    return hr;
}

static HRESULT WINAPI ivbsaxlocator_GetIDsOfNames(
    IVBSAXLocator *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%s %p %u %u %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    if(!rgszNames || cNames == 0 || !rgDispId)
        return E_INVALIDARG;

    hr = get_typeinfo(IVBSAXLocator_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames(typeinfo, rgszNames, cNames, rgDispId);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

static HRESULT WINAPI ivbsaxlocator_Invoke(
    IVBSAXLocator *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%d %s %d %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(IVBSAXLocator_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke(typeinfo, &This->IVBSAXLocator_iface, dispIdMember, wFlags,
                pDispParams, pVarResult, pExcepInfo, puArgErr);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

/*** IVBSAXLocator methods ***/
static HRESULT WINAPI ivbsaxlocator_get_columnNumber(
        IVBSAXLocator* iface,
        int *pnColumn)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    return ISAXLocator_getColumnNumber((ISAXLocator*)&This->IVBSAXLocator_iface, pnColumn);
}

static HRESULT WINAPI ivbsaxlocator_get_lineNumber(
        IVBSAXLocator* iface,
        int *pnLine)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    return ISAXLocator_getLineNumber((ISAXLocator*)&This->IVBSAXLocator_iface, pnLine);
}

static HRESULT WINAPI ivbsaxlocator_get_publicId(
        IVBSAXLocator* iface,
        BSTR* publicId)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    return ISAXLocator_getPublicId((ISAXLocator*)&This->IVBSAXLocator_iface,
            (const WCHAR**)publicId);
}

static HRESULT WINAPI ivbsaxlocator_get_systemId(
        IVBSAXLocator* iface,
        BSTR* systemId)
{
    saxlocator *This = impl_from_IVBSAXLocator( iface );
    return ISAXLocator_getSystemId((ISAXLocator*)&This->IVBSAXLocator_iface,
            (const WCHAR**)systemId);
}

static const struct IVBSAXLocatorVtbl VBSAXLocatorVtbl =
{
    ivbsaxlocator_QueryInterface,
    ivbsaxlocator_AddRef,
    ivbsaxlocator_Release,
    ivbsaxlocator_GetTypeInfoCount,
    ivbsaxlocator_GetTypeInfo,
    ivbsaxlocator_GetIDsOfNames,
    ivbsaxlocator_Invoke,
    ivbsaxlocator_get_columnNumber,
    ivbsaxlocator_get_lineNumber,
    ivbsaxlocator_get_publicId,
    ivbsaxlocator_get_systemId
};

/*** ISAXLocator interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI isaxlocator_QueryInterface(ISAXLocator* iface, REFIID riid, void **ppvObject)
{
    saxlocator *This = impl_from_ISAXLocator( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), ppvObject );

    *ppvObject = NULL;

    if ( IsEqualGUID( riid, &IID_IUnknown ) ||
            IsEqualGUID( riid, &IID_ISAXLocator ))
    {
        *ppvObject = iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXAttributes ))
    {
        *ppvObject = &This->ISAXAttributes_iface;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    ISAXLocator_AddRef( iface );

    return S_OK;
}

static ULONG WINAPI isaxlocator_AddRef(ISAXLocator* iface)
{
    saxlocator *This = impl_from_ISAXLocator( iface );
    ULONG ref = InterlockedIncrement( &This->ref );
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI isaxlocator_Release(
        ISAXLocator* iface)
{
    saxlocator *This = impl_from_ISAXLocator( iface );
    LONG ref = InterlockedDecrement( &This->ref );

    TRACE("(%p)->(%d)\n", This, ref );

    if (ref == 0)
    {
        element_entry *element, *element2;
        int index;

        SysFreeString(This->publicId);
        SysFreeString(This->systemId);
        SysFreeString(This->namespaceUri);

        for(index=0; index<This->nb_attributes; index++)
        {
            SysFreeString(This->attributes[index].szLocalname);
            SysFreeString(This->attributes[index].szValue);
            SysFreeString(This->attributes[index].szQName);
        }
        heap_free(This->attributes);

        /* element stack */
        LIST_FOR_EACH_ENTRY_SAFE(element, element2, &This->elements, element_entry, entry)
        {
            list_remove(&element->entry);
            free_element_entry(element);
        }

        ISAXXMLReader_Release(&This->saxreader->ISAXXMLReader_iface);
        heap_free( This );
    }

    return ref;
}

/*** ISAXLocator methods ***/
static HRESULT WINAPI isaxlocator_getColumnNumber(
        ISAXLocator* iface,
        int *pnColumn)
{
    saxlocator *This = impl_from_ISAXLocator( iface );

    *pnColumn = This->column;
    return S_OK;
}

static HRESULT WINAPI isaxlocator_getLineNumber(
        ISAXLocator* iface,
        int *pnLine)
{
    saxlocator *This = impl_from_ISAXLocator( iface );

    *pnLine = This->line;
    return S_OK;
}

static HRESULT WINAPI isaxlocator_getPublicId(
        ISAXLocator* iface,
        const WCHAR ** ppwchPublicId)
{
    BSTR publicId;
    saxlocator *This = impl_from_ISAXLocator( iface );

    SysFreeString(This->publicId);

    publicId = bstr_from_xmlChar(xmlSAX2GetPublicId(This->pParserCtxt));
    if(SysStringLen(publicId))
        This->publicId = (WCHAR*)&publicId;
    else
    {
        SysFreeString(publicId);
        This->publicId = NULL;
    }

    *ppwchPublicId = This->publicId;
    return S_OK;
}

static HRESULT WINAPI isaxlocator_getSystemId(
        ISAXLocator* iface,
        const WCHAR ** ppwchSystemId)
{
    BSTR systemId;
    saxlocator *This = impl_from_ISAXLocator( iface );

    SysFreeString(This->systemId);

    systemId = bstr_from_xmlChar(xmlSAX2GetSystemId(This->pParserCtxt));
    if(SysStringLen(systemId))
        This->systemId = (WCHAR*)&systemId;
    else
    {
        SysFreeString(systemId);
        This->systemId = NULL;
    }

    *ppwchSystemId = This->systemId;
    return S_OK;
}

static const struct ISAXLocatorVtbl SAXLocatorVtbl =
{
    isaxlocator_QueryInterface,
    isaxlocator_AddRef,
    isaxlocator_Release,
    isaxlocator_getColumnNumber,
    isaxlocator_getLineNumber,
    isaxlocator_getPublicId,
    isaxlocator_getSystemId
};

static HRESULT SAXLocator_create(saxreader *reader, saxlocator **ppsaxlocator, BOOL vbInterface)
{
    static const WCHAR w3xmlns[] = { 'h','t','t','p',':','/','/', 'w','w','w','.','w','3','.',
        'o','r','g','/','2','0','0','0','/','x','m','l','n','s','/',0 };

    saxlocator *locator;

    locator = heap_alloc( sizeof (*locator) );
    if( !locator )
        return E_OUTOFMEMORY;

    locator->IVBSAXLocator_iface.lpVtbl = &VBSAXLocatorVtbl;
    locator->ISAXLocator_iface.lpVtbl = &SAXLocatorVtbl;
    locator->IVBSAXAttributes_iface.lpVtbl = &ivbsaxattributes_vtbl;
    locator->ISAXAttributes_iface.lpVtbl = &isaxattributes_vtbl;
    locator->ref = 1;
    locator->vbInterface = vbInterface;

    locator->saxreader = reader;
    ISAXXMLReader_AddRef(&reader->ISAXXMLReader_iface);

    locator->pParserCtxt = NULL;
    locator->publicId = NULL;
    locator->systemId = NULL;
    locator->line = (reader->version>=MSXML6 ? 1 : 0);
    locator->column = 0;
    locator->ret = S_OK;
    if(locator->saxreader->version >= MSXML6)
        locator->namespaceUri = SysAllocString(w3xmlns);
    else
        locator->namespaceUri = SysAllocStringLen(NULL, 0);
    if(!locator->namespaceUri)
    {
        ISAXXMLReader_Release(&reader->ISAXXMLReader_iface);
        heap_free(locator);
        return E_OUTOFMEMORY;
    }

    locator->attributesSize = 8;
    locator->nb_attributes = 0;
    locator->attributes = heap_alloc(sizeof(struct _attributes)*locator->attributesSize);
    if(!locator->attributes)
    {
        ISAXXMLReader_Release(&reader->ISAXXMLReader_iface);
        SysFreeString(locator->namespaceUri);
        heap_free(locator);
        return E_OUTOFMEMORY;
    }

    list_init(&locator->elements);

    *ppsaxlocator = locator;

    TRACE("returning %p\n", *ppsaxlocator);

    return S_OK;
}

/*** SAXXMLReader internal functions ***/
static HRESULT internal_parseBuffer(saxreader *This, const char *buffer, int size, BOOL vbInterface)
{
    xmlCharEncoding encoding = XML_CHAR_ENCODING_NONE;
    xmlChar *enc_name = NULL;
    saxlocator *locator;
    HRESULT hr;

    hr = SAXLocator_create(This, &locator, vbInterface);
    if(FAILED(hr))
        return hr;

    if (size >= 4)
    {
        const unsigned char *buff = (unsigned char*)buffer;

        encoding = xmlDetectCharEncoding((xmlChar*)buffer, 4);
        enc_name = (xmlChar*)xmlGetCharEncodingName(encoding);
        TRACE("detected encoding: %s\n", enc_name);
        /* skip BOM, parser won't switch encodings and so won't skip it on its own */
        if ((encoding == XML_CHAR_ENCODING_UTF8) &&
            buff[0] == 0xEF && buff[1] == 0xBB && buff[2] == 0xBF)
        {
            buffer += 3;
            size -= 3;
        }
    }

    locator->pParserCtxt = xmlCreateMemoryParserCtxt(buffer, size);
    if(!locator->pParserCtxt)
    {
        ISAXLocator_Release(&locator->ISAXLocator_iface);
        return E_FAIL;
    }

    if (encoding == XML_CHAR_ENCODING_UTF8)
        locator->pParserCtxt->encoding = xmlStrdup(enc_name);

    xmlFree(locator->pParserCtxt->sax);
    locator->pParserCtxt->sax = &locator->saxreader->sax;
    locator->pParserCtxt->userData = locator;

    This->isParsing = TRUE;
    if(xmlParseDocument(locator->pParserCtxt)==-1 && locator->ret==S_OK)
        hr = E_FAIL;
    else
        hr = locator->ret;
    This->isParsing = FALSE;

    if(locator->pParserCtxt)
    {
        locator->pParserCtxt->sax = NULL;
        xmlFreeParserCtxt(locator->pParserCtxt);
        locator->pParserCtxt = NULL;
    }

    ISAXLocator_Release(&locator->ISAXLocator_iface);
    return hr;
}

static HRESULT internal_parseStream(saxreader *This, IStream *stream, BOOL vbInterface)
{
    saxlocator *locator;
    HRESULT hr;
    ULONG dataRead;
    char data[1024];
    int ret;

    dataRead = 0;
    hr = IStream_Read(stream, data, sizeof(data), &dataRead);
    if(FAILED(hr)) return hr;

    hr = SAXLocator_create(This, &locator, vbInterface);
    if(FAILED(hr)) return hr;

    locator->pParserCtxt = xmlCreatePushParserCtxt(
            &locator->saxreader->sax, locator,
            data, dataRead, NULL);
    if(!locator->pParserCtxt)
    {
        ISAXLocator_Release(&locator->ISAXLocator_iface);
        return E_FAIL;
    }

    This->isParsing = TRUE;

    if(dataRead != sizeof(data))
    {
        ret = xmlParseChunk(locator->pParserCtxt, data, 0, 1);
        hr = ret!=XML_ERR_OK && locator->ret==S_OK ? E_FAIL : locator->ret;
    }
    else
    {
        while(1)
        {
            dataRead = 0;
            hr = IStream_Read(stream, data, sizeof(data), &dataRead);
            if (FAILED(hr)) break;

            ret = xmlParseChunk(locator->pParserCtxt, data, dataRead, 0);
            hr = ret!=XML_ERR_OK && locator->ret==S_OK ? E_FAIL : locator->ret;

            if (hr != S_OK) break;

            if (dataRead != sizeof(data))
            {
                ret = xmlParseChunk(locator->pParserCtxt, data, 0, 1);
                hr = ret!=XML_ERR_OK && locator->ret==S_OK ? E_FAIL : locator->ret;
                break;
            }
        }
    }

    This->isParsing = FALSE;

    xmlFreeParserCtxt(locator->pParserCtxt);
    locator->pParserCtxt = NULL;
    ISAXLocator_Release(&locator->ISAXLocator_iface);
    return hr;
}

static HRESULT internal_getEntityResolver(
        saxreader *This,
        void *pEntityResolver,
        BOOL vbInterface)
{
    FIXME("(%p)->(%p) stub\n", This, pEntityResolver);
    return E_NOTIMPL;
}

static HRESULT internal_putEntityResolver(
        saxreader *This,
        void *pEntityResolver,
        BOOL vbInterface)
{
    FIXME("(%p)->(%p) stub\n", This, pEntityResolver);
    return E_NOTIMPL;
}

static HRESULT internal_getContentHandler(
        saxreader* This,
        void *pContentHandler,
        BOOL vbInterface)
{
    TRACE("(%p)->(%p)\n", This, pContentHandler);
    if(pContentHandler == NULL)
        return E_POINTER;
    if((vbInterface && This->vbcontentHandler)
            || (!vbInterface && This->contentHandler))
    {
        if(vbInterface)
            IVBSAXContentHandler_AddRef(This->vbcontentHandler);
        else
            ISAXContentHandler_AddRef(This->contentHandler);
    }
    if(vbInterface) *(IVBSAXContentHandler**)pContentHandler =
        This->vbcontentHandler;
    else *(ISAXContentHandler**)pContentHandler = This->contentHandler;

    return S_OK;
}

static HRESULT internal_putContentHandler(
        saxreader* This,
        void *contentHandler,
        BOOL vbInterface)
{
    TRACE("(%p)->(%p)\n", This, contentHandler);
    if(contentHandler)
    {
        if(vbInterface)
            IVBSAXContentHandler_AddRef((IVBSAXContentHandler*)contentHandler);
        else
            ISAXContentHandler_AddRef((ISAXContentHandler*)contentHandler);
    }
    if((vbInterface && This->vbcontentHandler)
            || (!vbInterface && This->contentHandler))
    {
        if(vbInterface)
            IVBSAXContentHandler_Release(This->vbcontentHandler);
        else
            ISAXContentHandler_Release(This->contentHandler);
    }
    if(vbInterface)
        This->vbcontentHandler = contentHandler;
    else
        This->contentHandler = contentHandler;

    return S_OK;
}

static HRESULT internal_getDTDHandler(
        saxreader* This,
        void *pDTDHandler,
        BOOL vbInterface)
{
    FIXME("(%p)->(%p) stub\n", This, pDTDHandler);
    return E_NOTIMPL;
}

static HRESULT internal_putDTDHandler(
        saxreader* This,
        void *pDTDHandler,
        BOOL vbInterface)
{
    FIXME("(%p)->(%p) stub\n", This, pDTDHandler);
    return E_NOTIMPL;
}

static HRESULT internal_getErrorHandler(
        saxreader* This,
        void *pErrorHandler,
        BOOL vbInterface)
{
    TRACE("(%p)->(%p)\n", This, pErrorHandler);
    if(pErrorHandler == NULL)
        return E_POINTER;

    if(vbInterface && This->vberrorHandler)
        IVBSAXErrorHandler_AddRef(This->vberrorHandler);
    else if(!vbInterface && This->errorHandler)
        ISAXErrorHandler_AddRef(This->errorHandler);

    if(vbInterface)
        *(IVBSAXErrorHandler**)pErrorHandler = This->vberrorHandler;
    else
        *(ISAXErrorHandler**)pErrorHandler = This->errorHandler;

    return S_OK;

}

static HRESULT internal_putErrorHandler(
        saxreader* This,
        void *errorHandler,
        BOOL vbInterface)
{
    TRACE("(%p)->(%p)\n", This, errorHandler);
    if(errorHandler)
    {
        if(vbInterface)
            IVBSAXErrorHandler_AddRef((IVBSAXErrorHandler*)errorHandler);
        else
            ISAXErrorHandler_AddRef((ISAXErrorHandler*)errorHandler);
    }

    if(vbInterface && This->vberrorHandler)
        IVBSAXErrorHandler_Release(This->vberrorHandler);
    else if(!vbInterface && This->errorHandler)
        ISAXErrorHandler_Release(This->errorHandler);

    if(vbInterface)
        This->vberrorHandler = errorHandler;
    else
        This->errorHandler = errorHandler;

    return S_OK;

}

static HRESULT internal_parse(
        saxreader* This,
        VARIANT varInput,
        BOOL vbInterface)
{
    HRESULT hr;

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&varInput));

    /* Dispose of the BSTRs in the pool from a prior run, if any. */
    free_bstr_pool(&This->pool);

    switch(V_VT(&varInput))
    {
        case VT_BSTR:
            hr = internal_parseBuffer(This, (const char*)V_BSTR(&varInput),
                    SysStringByteLen(V_BSTR(&varInput)), vbInterface);
            break;
        case VT_ARRAY|VT_UI1: {
            void *pSAData;
            LONG lBound, uBound;
            ULONG dataRead;

            hr = SafeArrayGetLBound(V_ARRAY(&varInput), 1, &lBound);
            if(hr != S_OK) break;
            hr = SafeArrayGetUBound(V_ARRAY(&varInput), 1, &uBound);
            if(hr != S_OK) break;
            dataRead = (uBound-lBound)*SafeArrayGetElemsize(V_ARRAY(&varInput));
            hr = SafeArrayAccessData(V_ARRAY(&varInput), &pSAData);
            if(hr != S_OK) break;
            hr = internal_parseBuffer(This, pSAData, dataRead, vbInterface);
            SafeArrayUnaccessData(V_ARRAY(&varInput));
            break;
        }
        case VT_UNKNOWN:
        case VT_DISPATCH: {
            IPersistStream *persistStream;
            IStream *stream = NULL;
            IXMLDOMDocument *xmlDoc;

            if(IUnknown_QueryInterface(V_UNKNOWN(&varInput),
                        &IID_IXMLDOMDocument, (void**)&xmlDoc) == S_OK)
            {
                BSTR bstrData;

                IXMLDOMDocument_get_xml(xmlDoc, &bstrData);
                hr = internal_parseBuffer(This, (const char*)bstrData,
                        SysStringByteLen(bstrData), vbInterface);
                IXMLDOMDocument_Release(xmlDoc);
                SysFreeString(bstrData);
                break;
            }

            if(IUnknown_QueryInterface(V_UNKNOWN(&varInput),
                        &IID_IPersistStream, (void**)&persistStream) == S_OK)
            {
                hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
                if(hr != S_OK)
                {
                    IPersistStream_Release(persistStream);
                    return hr;
                }

                hr = IPersistStream_Save(persistStream, stream, TRUE);
                IPersistStream_Release(persistStream);
                if(hr != S_OK)
                {
                    IStream_Release(stream);
                    break;
                }
            }
            if(stream || IUnknown_QueryInterface(V_UNKNOWN(&varInput),
                        &IID_IStream, (void**)&stream) == S_OK)
            {
                hr = internal_parseStream(This, stream, vbInterface);
                IStream_Release(stream);
                break;
            }
        }
        default:
            WARN("vt %d not implemented\n", V_VT(&varInput));
            hr = E_INVALIDARG;
    }

    return hr;
}

static HRESULT internal_vbonDataAvailable(void *obj, char *ptr, DWORD len)
{
    saxreader *This = obj;

    return internal_parseBuffer(This, ptr, len, TRUE);
}

static HRESULT internal_onDataAvailable(void *obj, char *ptr, DWORD len)
{
    saxreader *This = obj;

    return internal_parseBuffer(This, ptr, len, FALSE);
}

static HRESULT internal_parseURL(
        saxreader* This,
        const WCHAR *url,
        BOOL vbInterface)
{
    bsc_t *bsc;
    HRESULT hr;

    TRACE("(%p)->(%s)\n", This, debugstr_w(url));

    if(vbInterface) hr = bind_url(url, internal_vbonDataAvailable, This, &bsc);
    else hr = bind_url(url, internal_onDataAvailable, This, &bsc);

    if(FAILED(hr))
        return hr;

    return detach_bsc(bsc);
}

static HRESULT internal_putProperty(
    saxreader* This,
    const WCHAR *prop,
    VARIANT value,
    BOOL vbInterface)
{
    TRACE("(%p)->(%s %s)\n", This, debugstr_w(prop), debugstr_variant(&value));

    if(!memcmp(prop, PropertyDeclHandlerW, sizeof(PropertyDeclHandlerW)))
    {
        if(This->isParsing) return E_FAIL;

        switch (V_VT(&value))
        {
        case VT_EMPTY:
            if (vbInterface)
            {
                if (This->vbdeclHandler)
                {
                    IVBSAXDeclHandler_Release(This->vbdeclHandler);
                    This->vbdeclHandler = NULL;
                }
            }
            else
                if (This->declHandler)
                {
                    ISAXDeclHandler_Release(This->declHandler);
                    This->declHandler = NULL;
                }
            break;
        case VT_UNKNOWN:
            if (V_UNKNOWN(&value)) IUnknown_AddRef(V_UNKNOWN(&value));

            if ((vbInterface && This->vbdeclHandler) ||
               (!vbInterface && This->declHandler))
            {
                if (vbInterface)
                    IVBSAXDeclHandler_Release(This->vbdeclHandler);
                else
                    ISAXDeclHandler_Release(This->declHandler);
            }

            if (vbInterface)
                This->vbdeclHandler = (IVBSAXDeclHandler*)V_UNKNOWN(&value);
            else
                This->declHandler = (ISAXDeclHandler*)V_UNKNOWN(&value);
            break;
        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }

    if(!memcmp(prop, PropertyLexicalHandlerW, sizeof(PropertyLexicalHandlerW)))
    {
        if(This->isParsing) return E_FAIL;

        switch (V_VT(&value))
        {
        case VT_EMPTY:
            if (vbInterface)
            {
                if (This->vblexicalHandler)
                {
                    IVBSAXLexicalHandler_Release(This->vblexicalHandler);
                    This->vblexicalHandler = NULL;
                }
            }
            else
                if (This->lexicalHandler)
                {
                    ISAXLexicalHandler_Release(This->lexicalHandler);
                    This->lexicalHandler = NULL;
                }
            break;
        case VT_UNKNOWN:
            if (V_UNKNOWN(&value)) IUnknown_AddRef(V_UNKNOWN(&value));

            if ((vbInterface && This->vblexicalHandler) ||
               (!vbInterface && This->lexicalHandler))
            {
                if (vbInterface)
                    IVBSAXLexicalHandler_Release(This->vblexicalHandler);
                else
                    ISAXLexicalHandler_Release(This->lexicalHandler);
            }

            if (vbInterface)
                This->vblexicalHandler = (IVBSAXLexicalHandler*)V_UNKNOWN(&value);
            else
                This->lexicalHandler = (ISAXLexicalHandler*)V_UNKNOWN(&value);
            break;
        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }

    if(!memcmp(prop, PropertyMaxXMLSizeW, sizeof(PropertyMaxXMLSizeW)))
    {
        if (V_VT(&value) == VT_I4 && V_I4(&value) == 0) return S_OK;
        FIXME("(%p)->(%s): max-xml-size unsupported\n", This, debugstr_variant(&value));
        return E_NOTIMPL;
    }

    if(!memcmp(prop, PropertyMaxElementDepthW, sizeof(PropertyMaxElementDepthW)))
    {
        if (V_VT(&value) == VT_I4 && V_I4(&value) == 0) return S_OK;
        FIXME("(%p)->(%s): max-element-depth unsupported\n", This, debugstr_variant(&value));
        return E_NOTIMPL;
    }

    FIXME("(%p)->(%s:%s): unsupported property\n", This, debugstr_w(prop), debugstr_variant(&value));

    if(!memcmp(prop, PropertyCharsetW, sizeof(PropertyCharsetW)))
        return E_NOTIMPL;

    if(!memcmp(prop, PropertyDomNodeW, sizeof(PropertyDomNodeW)))
        return E_FAIL;

    if(!memcmp(prop, PropertyInputSourceW, sizeof(PropertyInputSourceW)))
        return E_NOTIMPL;

    if(!memcmp(prop, PropertySchemaDeclHandlerW, sizeof(PropertySchemaDeclHandlerW)))
        return E_NOTIMPL;

    if(!memcmp(prop, PropertyXMLDeclEncodingW, sizeof(PropertyXMLDeclEncodingW)))
        return E_FAIL;

    if(!memcmp(prop, PropertyXMLDeclStandaloneW, sizeof(PropertyXMLDeclStandaloneW)))
        return E_FAIL;

    if(!memcmp(prop, PropertyXMLDeclVersionW, sizeof(PropertyXMLDeclVersionW)))
        return E_FAIL;

    return E_INVALIDARG;
}

static HRESULT internal_getProperty(const saxreader* This, const WCHAR *prop, VARIANT *value, BOOL vb)
{
    TRACE("(%p)->(%s)\n", This, debugstr_w(prop));

    if (!value) return E_POINTER;

    if (!memcmp(PropertyLexicalHandlerW, prop, sizeof(PropertyLexicalHandlerW)))
    {
        V_VT(value) = VT_UNKNOWN;
        V_UNKNOWN(value) = vb ? (IUnknown*)This->vblexicalHandler : (IUnknown*)This->lexicalHandler;
        if (V_UNKNOWN(value)) IUnknown_AddRef(V_UNKNOWN(value));
        return S_OK;
    }

    if (!memcmp(PropertyDeclHandlerW, prop, sizeof(PropertyDeclHandlerW)))
    {
        V_VT(value) = VT_UNKNOWN;
        V_UNKNOWN(value) = vb ? (IUnknown*)This->vbdeclHandler : (IUnknown*)This->declHandler;
        if (V_UNKNOWN(value)) IUnknown_AddRef(V_UNKNOWN(value));
        return S_OK;
    }

    FIXME("(%p)->(%s) unsupported property\n", This, debugstr_w(prop));

    return E_NOTIMPL;
}

/*** IVBSAXXMLReader interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI saxxmlreader_QueryInterface(IVBSAXXMLReader* iface, REFIID riid, void **ppvObject)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    TRACE("%p %s %p\n", This, debugstr_guid( riid ), ppvObject );

    *ppvObject = NULL;

    if ( IsEqualGUID( riid, &IID_IUnknown ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IVBSAXXMLReader ))
    {
        *ppvObject = iface;
    }
    else if( IsEqualGUID( riid, &IID_ISAXXMLReader ))
    {
        *ppvObject = &This->ISAXXMLReader_iface;
    }
    else if (dispex_query_interface(&This->dispex, riid, ppvObject))
    {
        return *ppvObject ? S_OK : E_NOINTERFACE;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    IVBSAXXMLReader_AddRef( iface );

    return S_OK;
}

static ULONG WINAPI saxxmlreader_AddRef(IVBSAXXMLReader* iface)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    TRACE("%p\n", This );
    return InterlockedIncrement( &This->ref );
}

static ULONG WINAPI saxxmlreader_Release(
    IVBSAXXMLReader* iface)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    LONG ref;

    TRACE("%p\n", This );

    ref = InterlockedDecrement( &This->ref );
    if ( ref == 0 )
    {
        if(This->contentHandler)
            ISAXContentHandler_Release(This->contentHandler);

        if(This->vbcontentHandler)
            IVBSAXContentHandler_Release(This->vbcontentHandler);

        if(This->errorHandler)
            ISAXErrorHandler_Release(This->errorHandler);

        if(This->vberrorHandler)
            IVBSAXErrorHandler_Release(This->vberrorHandler);

        if(This->lexicalHandler)
            ISAXLexicalHandler_Release(This->lexicalHandler);

        if(This->vblexicalHandler)
            IVBSAXLexicalHandler_Release(This->vblexicalHandler);

        if(This->declHandler)
            ISAXDeclHandler_Release(This->declHandler);

        if(This->vbdeclHandler)
            IVBSAXDeclHandler_Release(This->vbdeclHandler);

        free_bstr_pool(&This->pool);

        release_dispex(&This->dispex);
        heap_free( This );
    }

    return ref;
}
/*** IDispatch ***/
static HRESULT WINAPI saxxmlreader_GetTypeInfoCount( IVBSAXXMLReader *iface, UINT* pctinfo )
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI saxxmlreader_GetTypeInfo(
    IVBSAXXMLReader *iface,
    UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo )
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface,
        iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI saxxmlreader_GetIDsOfNames(
    IVBSAXXMLReader *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface,
        riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI saxxmlreader_Invoke(
    IVBSAXXMLReader *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface,
        dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

/*** IVBSAXXMLReader methods ***/
static HRESULT WINAPI saxxmlreader_getFeature(
    IVBSAXXMLReader* iface,
    const WCHAR *feature,
    VARIANT_BOOL *value)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    if (!strcmpW(FeatureNamespacesW, feature))
        return get_feature_value(This, Namespaces, value);

    FIXME("(%p)->(%s %p) stub\n", This, debugstr_w(feature), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI saxxmlreader_putFeature(
    IVBSAXXMLReader* iface,
    const WCHAR *feature,
    VARIANT_BOOL value)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    TRACE("(%p)->(%s %x)\n", This, debugstr_w(feature), value);

    if (!strcmpW(FeatureExternalGeneralEntitiesW, feature) && value == VARIANT_FALSE)
        return set_feature_value(This, ExternalGeneralEntities, value);

    if (!strcmpW(FeatureExternalParameterEntitiesW, feature) && value == VARIANT_FALSE)
        return set_feature_value(This, ExternalParameterEntities, value);

    if (!strcmpW(FeatureLexicalHandlerParEntitiesW, feature))
    {
        FIXME("(%p)->(%s %x) stub\n", This, debugstr_w(feature), value);
        return set_feature_value(This, LexicalHandlerParEntities, value);
    }

    if (!strcmpW(FeatureProhibitDTDW, feature))
    {
        FIXME("(%p)->(%s %x) stub\n", This, debugstr_w(feature), value);
        return set_feature_value(This, ProhibitDTD, value);
    }

    if (!strcmpW(FeatureNamespacesW, feature) && value == VARIANT_TRUE)
        return set_feature_value(This, Namespaces, value);

    FIXME("(%p)->(%s %x) stub\n", This, debugstr_w(feature), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI saxxmlreader_getProperty(
    IVBSAXXMLReader* iface,
    const WCHAR *prop,
    VARIANT *value)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_getProperty(This, prop, value, TRUE);
}

static HRESULT WINAPI saxxmlreader_putProperty(
    IVBSAXXMLReader* iface,
    const WCHAR *pProp,
    VARIANT value)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_putProperty(This, pProp, value, TRUE);
}

static HRESULT WINAPI saxxmlreader_get_entityResolver(
    IVBSAXXMLReader* iface,
    IVBSAXEntityResolver **pEntityResolver)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_getEntityResolver(This, pEntityResolver, TRUE);
}

static HRESULT WINAPI saxxmlreader_put_entityResolver(
    IVBSAXXMLReader* iface,
    IVBSAXEntityResolver *pEntityResolver)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_putEntityResolver(This, pEntityResolver, TRUE);
}

static HRESULT WINAPI saxxmlreader_get_contentHandler(
    IVBSAXXMLReader* iface,
    IVBSAXContentHandler **ppContentHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_getContentHandler(This, ppContentHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_put_contentHandler(
    IVBSAXXMLReader* iface,
    IVBSAXContentHandler *contentHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_putContentHandler(This, contentHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_get_dtdHandler(
    IVBSAXXMLReader* iface,
    IVBSAXDTDHandler **pDTDHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_getDTDHandler(This, pDTDHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_put_dtdHandler(
    IVBSAXXMLReader* iface,
    IVBSAXDTDHandler *pDTDHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_putDTDHandler(This, pDTDHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_get_errorHandler(
    IVBSAXXMLReader* iface,
    IVBSAXErrorHandler **pErrorHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_getErrorHandler(This, pErrorHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_put_errorHandler(
    IVBSAXXMLReader* iface,
    IVBSAXErrorHandler *errorHandler)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_putErrorHandler(This, errorHandler, TRUE);
}

static HRESULT WINAPI saxxmlreader_get_baseURL(
    IVBSAXXMLReader* iface,
    const WCHAR **pBaseUrl)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    FIXME("(%p)->(%p) stub\n", This, pBaseUrl);
    return E_NOTIMPL;
}

static HRESULT WINAPI saxxmlreader_put_baseURL(
    IVBSAXXMLReader* iface,
    const WCHAR *pBaseUrl)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    FIXME("(%p)->(%s) stub\n", This, debugstr_w(pBaseUrl));
    return E_NOTIMPL;
}

static HRESULT WINAPI saxxmlreader_get_secureBaseURL(
    IVBSAXXMLReader* iface,
    const WCHAR **pSecureBaseUrl)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    FIXME("(%p)->(%p) stub\n", This, pSecureBaseUrl);
    return E_NOTIMPL;
}


static HRESULT WINAPI saxxmlreader_put_secureBaseURL(
    IVBSAXXMLReader* iface,
    const WCHAR *secureBaseUrl)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );

    FIXME("(%p)->(%s) stub\n", This, debugstr_w(secureBaseUrl));
    return E_NOTIMPL;
}

static HRESULT WINAPI saxxmlreader_parse(
    IVBSAXXMLReader* iface,
    VARIANT varInput)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_parse(This, varInput, TRUE);
}

static HRESULT WINAPI saxxmlreader_parseURL(
    IVBSAXXMLReader* iface,
    const WCHAR *url)
{
    saxreader *This = impl_from_IVBSAXXMLReader( iface );
    return internal_parseURL(This, url, TRUE);
}

static const struct IVBSAXXMLReaderVtbl VBSAXXMLReaderVtbl =
{
    saxxmlreader_QueryInterface,
    saxxmlreader_AddRef,
    saxxmlreader_Release,
    saxxmlreader_GetTypeInfoCount,
    saxxmlreader_GetTypeInfo,
    saxxmlreader_GetIDsOfNames,
    saxxmlreader_Invoke,
    saxxmlreader_getFeature,
    saxxmlreader_putFeature,
    saxxmlreader_getProperty,
    saxxmlreader_putProperty,
    saxxmlreader_get_entityResolver,
    saxxmlreader_put_entityResolver,
    saxxmlreader_get_contentHandler,
    saxxmlreader_put_contentHandler,
    saxxmlreader_get_dtdHandler,
    saxxmlreader_put_dtdHandler,
    saxxmlreader_get_errorHandler,
    saxxmlreader_put_errorHandler,
    saxxmlreader_get_baseURL,
    saxxmlreader_put_baseURL,
    saxxmlreader_get_secureBaseURL,
    saxxmlreader_put_secureBaseURL,
    saxxmlreader_parse,
    saxxmlreader_parseURL
};

/*** ISAXXMLReader interface ***/
/*** IUnknown methods ***/
static HRESULT WINAPI isaxxmlreader_QueryInterface(ISAXXMLReader* iface, REFIID riid, void **ppvObject)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return saxxmlreader_QueryInterface(&This->IVBSAXXMLReader_iface, riid, ppvObject);
}

static ULONG WINAPI isaxxmlreader_AddRef(ISAXXMLReader* iface)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return saxxmlreader_AddRef(&This->IVBSAXXMLReader_iface);
}

static ULONG WINAPI isaxxmlreader_Release(ISAXXMLReader* iface)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return saxxmlreader_Release(&This->IVBSAXXMLReader_iface);
}

/*** ISAXXMLReader methods ***/
static HRESULT WINAPI isaxxmlreader_getFeature(
        ISAXXMLReader* iface,
        const WCHAR *pFeature,
        VARIANT_BOOL *pValue)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_getFeature(&This->IVBSAXXMLReader_iface, pFeature, pValue);
}

static HRESULT WINAPI isaxxmlreader_putFeature(
        ISAXXMLReader* iface,
        const WCHAR *pFeature,
        VARIANT_BOOL vfValue)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_putFeature(&This->IVBSAXXMLReader_iface, pFeature, vfValue);
}

static HRESULT WINAPI isaxxmlreader_getProperty(
        ISAXXMLReader* iface,
        const WCHAR *prop,
        VARIANT *value)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_getProperty(This, prop, value, FALSE);
}

static HRESULT WINAPI isaxxmlreader_putProperty(
        ISAXXMLReader* iface,
        const WCHAR *pProp,
        VARIANT value)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_putProperty(This, pProp, value, FALSE);
}

static HRESULT WINAPI isaxxmlreader_getEntityResolver(
        ISAXXMLReader* iface,
        ISAXEntityResolver **ppEntityResolver)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_getEntityResolver(This, ppEntityResolver, FALSE);
}

static HRESULT WINAPI isaxxmlreader_putEntityResolver(
        ISAXXMLReader* iface,
        ISAXEntityResolver *pEntityResolver)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_putEntityResolver(This, pEntityResolver, FALSE);
}

static HRESULT WINAPI isaxxmlreader_getContentHandler(
        ISAXXMLReader* iface,
        ISAXContentHandler **pContentHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_getContentHandler(This, pContentHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_putContentHandler(
        ISAXXMLReader* iface,
        ISAXContentHandler *contentHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_putContentHandler(This, contentHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_getDTDHandler(
        ISAXXMLReader* iface,
        ISAXDTDHandler **pDTDHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_getDTDHandler(This, pDTDHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_putDTDHandler(
        ISAXXMLReader* iface,
        ISAXDTDHandler *pDTDHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_putDTDHandler(This, pDTDHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_getErrorHandler(
        ISAXXMLReader* iface,
        ISAXErrorHandler **pErrorHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_getErrorHandler(This, pErrorHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_putErrorHandler(
        ISAXXMLReader* iface,
        ISAXErrorHandler *errorHandler)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_putErrorHandler(This, errorHandler, FALSE);
}

static HRESULT WINAPI isaxxmlreader_getBaseURL(
        ISAXXMLReader* iface,
        const WCHAR **pBaseUrl)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_get_baseURL(&This->IVBSAXXMLReader_iface, pBaseUrl);
}

static HRESULT WINAPI isaxxmlreader_putBaseURL(
        ISAXXMLReader* iface,
        const WCHAR *pBaseUrl)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_put_baseURL(&This->IVBSAXXMLReader_iface, pBaseUrl);
}

static HRESULT WINAPI isaxxmlreader_getSecureBaseURL(
        ISAXXMLReader* iface,
        const WCHAR **pSecureBaseUrl)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_get_secureBaseURL(&This->IVBSAXXMLReader_iface, pSecureBaseUrl);
}

static HRESULT WINAPI isaxxmlreader_putSecureBaseURL(
        ISAXXMLReader* iface,
        const WCHAR *secureBaseUrl)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return IVBSAXXMLReader_put_secureBaseURL(&This->IVBSAXXMLReader_iface, secureBaseUrl);
}

static HRESULT WINAPI isaxxmlreader_parse(
        ISAXXMLReader* iface,
        VARIANT varInput)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_parse(This, varInput, FALSE);
}

static HRESULT WINAPI isaxxmlreader_parseURL(
        ISAXXMLReader* iface,
        const WCHAR *url)
{
    saxreader *This = impl_from_ISAXXMLReader( iface );
    return internal_parseURL(This, url, FALSE);
}

static const struct ISAXXMLReaderVtbl SAXXMLReaderVtbl =
{
    isaxxmlreader_QueryInterface,
    isaxxmlreader_AddRef,
    isaxxmlreader_Release,
    isaxxmlreader_getFeature,
    isaxxmlreader_putFeature,
    isaxxmlreader_getProperty,
    isaxxmlreader_putProperty,
    isaxxmlreader_getEntityResolver,
    isaxxmlreader_putEntityResolver,
    isaxxmlreader_getContentHandler,
    isaxxmlreader_putContentHandler,
    isaxxmlreader_getDTDHandler,
    isaxxmlreader_putDTDHandler,
    isaxxmlreader_getErrorHandler,
    isaxxmlreader_putErrorHandler,
    isaxxmlreader_getBaseURL,
    isaxxmlreader_putBaseURL,
    isaxxmlreader_getSecureBaseURL,
    isaxxmlreader_putSecureBaseURL,
    isaxxmlreader_parse,
    isaxxmlreader_parseURL
};

static const tid_t saxreader_iface_tids[] = {
    IVBSAXXMLReader_tid,
    0
};
static dispex_static_data_t saxreader_dispex = {
    NULL,
    IVBSAXXMLReader_tid,
    NULL,
    saxreader_iface_tids
};

HRESULT SAXXMLReader_create(MSXML_VERSION version, IUnknown *outer, LPVOID *ppObj)
{
    saxreader *reader;

    TRACE("(%p, %p)\n", outer, ppObj);

    reader = heap_alloc( sizeof (*reader) );
    if( !reader )
        return E_OUTOFMEMORY;

    reader->IVBSAXXMLReader_iface.lpVtbl = &VBSAXXMLReaderVtbl;
    reader->ISAXXMLReader_iface.lpVtbl = &SAXXMLReaderVtbl;
    reader->ref = 1;
    reader->contentHandler = NULL;
    reader->vbcontentHandler = NULL;
    reader->errorHandler = NULL;
    reader->vberrorHandler = NULL;
    reader->lexicalHandler = NULL;
    reader->vblexicalHandler = NULL;
    reader->declHandler = NULL;
    reader->vbdeclHandler = NULL;
    reader->isParsing = FALSE;
    reader->pool.pool = NULL;
    reader->pool.index = 0;
    reader->pool.len = 0;
    reader->features = Namespaces;
    reader->version = version;

    init_dispex(&reader->dispex, (IUnknown*)&reader->IVBSAXXMLReader_iface, &saxreader_dispex);

    memset(&reader->sax, 0, sizeof(xmlSAXHandler));
    reader->sax.initialized = XML_SAX2_MAGIC;
    reader->sax.startDocument = libxmlStartDocument;
    reader->sax.endDocument = libxmlEndDocument;
    reader->sax.startElementNs = libxmlStartElementNS;
    reader->sax.endElementNs = libxmlEndElementNS;
    reader->sax.characters = libxmlCharacters;
    reader->sax.setDocumentLocator = libxmlSetDocumentLocator;
    reader->sax.comment = libxmlComment;
    reader->sax.error = libxmlFatalError;
    reader->sax.fatalError = libxmlFatalError;
    reader->sax.cdataBlock = libxmlCDataBlock;

    *ppObj = &reader->IVBSAXXMLReader_iface;

    TRACE("returning iface %p\n", *ppObj);

    return S_OK;
}

#else

HRESULT SAXXMLReader_create(MSXML_VERSION version, IUnknown *pUnkOuter, LPVOID *ppObj)
{
    MESSAGE("This program tried to use a SAX XML Reader object, but\n"
            "libxml2 support was not present at compile time.\n");
    return E_NOTIMPL;
}

#endif
