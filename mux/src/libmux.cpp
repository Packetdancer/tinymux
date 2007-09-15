/*! \file libmux.cpp
 * \brief Base-level module support
 *
 * $Id$
 *
 */

#include "copyright.h"
#include "autoconf.h"
#include "config.h"

#ifdef HAVE_DLOPEN
#include <dlfcn.h>
#endif // HAVE_DLOPEN

#include "libmux.h"

extern "C"
{
    typedef MUX_RESULT FPCANUNLOADNOW(void);
    typedef MUX_RESULT FPREGISTER(void);
    typedef MUX_RESULT FPUNREGISTER(void);
};

#ifdef WIN32
typedef HINSTANCE MODULE_HANDLE;
#define MOD_OPEN(m)  LoadLibrary(m)
#define MOD_SYM(h,s) GetProcAddress(h,s)
#define MOD_CLOSE(h) FreeLibrary(h)
#else
typedef void     *MODULE_HANDLE;
#define MOD_OPEN(m)  dlopen((char *)m, RTLD_LAZY)
#define MOD_SYM(h,s) dlsym(h,s)
#define MOD_CLOSE(h) dlclose(h)
#endif

typedef struct mod_info
{
    struct mod_info  *pNext;
    FPGETCLASSOBJECT *fpGetClassObject;
    FPCANUNLOADNOW   *fpCanUnloadNow;
    FPREGISTER       *fpRegister;
    FPUNREGISTER     *fpUnregister;
    MODULE_HANDLE    hInst;
    UTF8             *pModuleName;
#ifdef WIN32
    UTF16            *pFileName;
#else
    UTF8             *pFileName;
#endif
    bool             bLoaded;
} MODULE_INFO;

typedef struct
{
    CLASS_INFO    ci;
    MODULE_INFO  *pModule;
} CLASS_INFO_PRIVATE;

static MODULE_INFO *g_pModuleList = NULL;
static MODULE_INFO *g_pModuleLast = NULL;

static MODULE_INFO  g_MainModule =
{
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    false
};

static int                  g_nClasses = 0;
static int                  g_nClassesAllocated = 0;
static CLASS_INFO_PRIVATE  *g_pClasses = NULL;

static MODULE_INFO *g_pModule = NULL;

static int                  g_nInterfaces = 0;
static int                  g_nInterfacesAllocated = 0;
static INTERFACE_INFO      *g_pInterfaces = NULL;

static process_context g_ProcessContext = IsUninitialized;

// TODO: The uniqueness tests are probably too strong.  It may be desireable
// for several modules to offer an implementation for the same classes.  If
// so, the conflict could be determined by the order the modules are loaded.
//
// This opens the problem of whether the second-priority implementation of a
// class begins to appear if the first-priority implementation de-registers
// itself.
//
// For now, these extra tests may catch bugs, and we'll punt on these
// interesting questions.
//

/*! \brief Make private copy of string.
 *
 * \param  UTF8[]   String to copy.
 * \return          Pointer to private copy of string.
 */

static UTF8 *CopyUTF8(const UTF8 *pString)
{
    size_t n = strlen((const char *)pString);
    UTF8 *p = NULL;

    try
    {
        p = new UTF8[n+1];
    }
    catch (...)
    {
        ; // Nothing.
    }

    if (NULL != p)
    {
        memcpy(p, pString, n+1);
    }
    return p;
}

#ifdef WIN32
static UTF16 *CopyUTF16(const UTF16 *pString)
{
    size_t n = wcslen(pString);
    UTF16 *p = NULL;

    try
    {
        p = new UTF16[n+1];
    }
    catch (...)
    {
        ; // Nothing.
    }

    if (NULL != p)
    {
        memcpy(p, pString, (n+1) * sizeof(UTF16));
    }
    return p;
}
#endif // WIN32

/*! \brief Find the first class ID not less than the requested class id.
 *
 * The return value may be beyond the end of the array, so callers should check bounds.
 *
 * \param  cid  Class ID.
 * \return      Index into g_pClasses.
 */

static int ClassFind(MUX_CID cid)
{
    // Binary search for the class id.
    //
    int lo = 0;
    int mid;
    int hi = g_nClasses - 1;
    while (lo <= hi)
    {
        mid = ((hi - lo) >> 1) + lo;
        if (cid < g_pClasses[mid].ci.cid)
        {
            hi = mid - 1;
        }
        else if (g_pClasses[mid].ci.cid < cid)
        {
            lo = mid + 1;
        }
        else // (g_pClasses[mid].ci.cid == cid)
        {
            return mid;
        }
    }
    return lo;
}

/*! \brief Find which module implements a particular class id.
 *
 * Note that callers may need to test for MainModule and cannot assume the
 * returned module record is implemented in a module.
 *
 * \param  cid  Class ID.
 * \return      Pointer to module.
 */

static MODULE_INFO *ModuleFindFromCID(MUX_CID cid)
{
    int i = ClassFind(cid);
    if (  i < g_nClasses
       && g_pClasses[i].ci.cid == cid)
   {
        return g_pClasses[i].pModule;
    }
    return NULL;
}

/*! \brief Find module given its module name.
 *
 * Note that it is not possible to find the special-case module for the main
 * program (netmux or stubslave) this way.
 *
 * \param  UTF8[]    Module name.
 * \return           Corresponding module record or NULL if not found.
 */

static MODULE_INFO *ModuleFindFromName(const UTF8 aModuleName[])
{
    MODULE_INFO *pModule = g_pModuleList;
    while (NULL != pModule)
    {
        if (strcmp((const char *)aModuleName, (const char *)pModule->pModuleName) == 0)
        {
            return pModule;
        }
        pModule = pModule->pNext;
    }
    return NULL;
}

/*! \brief Find module given its filename.
 *
 * Note that it is not possible to find the special-case module for the main
 * program (netmux or stubslave) this way.
 *
 * \param  UTF8[]    File name.
 * \return           Corresponding module record or NULL if not found.
 */

#ifdef WIN32
static MODULE_INFO *ModuleFindFromFileName(const UTF16 aFileName[])
#else
static MODULE_INFO *ModuleFindFromFileName(const UTF8 aFileName[])
#endif
{
    MODULE_INFO *pModule = g_pModuleList;
    while (NULL != pModule)
    {
        if (strcmp((const char *)aFileName, (const char *)pModule->pFileName) == 0)
        {
            return pModule;
        }
        pModule = pModule->pNext;
    }
    return NULL;
}

#define MINIMUM_SIZE 8

static int GrowByFactor(int i)
{
    if (i < MINIMUM_SIZE)
    {
        return MINIMUM_SIZE;
    }
    else
    {
        return 2*i;
    }
}

/*! \brief Adds (class id, module) in table while maintain its order.
 *
 * This routine assumes the array is large enough to hold the addition.
 *
 * \param pci        Class-related attributes including cid.
 * \param pModule    Module that implements it.
 * \return           None.
 */

static void ClassAdd(CLASS_INFO *pci, MODULE_INFO *pModule)
{
    int i = ClassFind(pci->cid);
    if (  i < g_nClasses
       && g_pClasses[i].ci.cid == pci->cid)
    {
        return;
    }

    if (i != g_nClasses)
    {
        memmove( g_pClasses + i + 1,
                 g_pClasses + i,
                 (g_nClasses - i) * sizeof(CLASS_INFO_PRIVATE));
    }
    g_nClasses++;

    g_pClasses[i].ci = *pci;
    g_pClasses[i].pModule = pModule;
}

/*! \brief Removes a class id from the table while maintaining order.
 *
 * \param cid       Class ID
 * \return          None.
 */

static void ClassRemove(MUX_CID cid)
{
    int i = ClassFind(cid);
    if (  i < g_nClasses
       && g_pClasses[i].ci.cid == cid)
    {
        g_nClasses--;
        if (i != g_nClasses)
        {
            memmove( g_pClasses + i,
                     g_pClasses + i + 1,
                     (g_nClasses - i) * sizeof(CLASS_INFO_PRIVATE));
        }
    }
}

static int InterfaceFind(MUX_IID iid)
{
    // Binary search for the interface id.
    //
    int lo = 0;
    int mid;
    int hi = g_nInterfaces - 1;
    while (lo <= hi)
    {
        mid = ((hi - lo) >> 1) + lo;
        if (iid < g_pInterfaces[mid].iid)
        {
            hi = mid - 1;
        }
        else if (g_pInterfaces[mid].iid < iid)
        {
            lo = mid + 1;
        }
        else // (g_pInterfaces[mid].iid == iid)
        {
            return mid;
        }
    }
    return lo;
}

static void InterfaceAdd(INTERFACE_INFO *pii)
{
    int i = InterfaceFind(pii->iid);
    if (  i < g_nInterfaces
       && g_pInterfaces[i].iid == pii->iid)
    {
        return;
    }

    if (i != g_nInterfaces)
    {
        memmove( g_pInterfaces + i + 1,
                 g_pInterfaces + i,
                 (g_nInterfaces - i) * sizeof(INTERFACE_INFO));
    }
    g_nInterfaces++;

    g_pInterfaces[i] = *pii;
}

static void InterfaceRemove(MUX_IID iid)
{
    int i = InterfaceFind(iid);
    if (  i < g_nInterfaces
       && g_pInterfaces[i].iid == iid)
    {
        g_nInterfaces--;
        if (i != g_nInterfaces)
        {
            memmove( g_pInterfaces + i,
                     g_pInterfaces + i + 1,
                     (g_nInterfaces - i) * sizeof(INTERFACE_INFO));
        }
    }
}

/*! \brief Adds a module.
 *
 * \param aModuleName[]  Filename of Module
 * \return               Module context record, NULL if out of memory or
 *                       duplicate found.
 */

#ifdef WIN32
static MODULE_INFO *ModuleAdd(const UTF8 aModuleName[], const UTF16 aFileName[])
#else
static MODULE_INFO *ModuleAdd(const UTF8 aModuleName[], const UTF8 aFileName[])
#endif
{
    // If the module name or file name is already being used, we won't add it
    // again.  This does not handle file-system links, but that will be caught
    // when the module tries to register its class ids.
    //
    MODULE_INFO *pModuleFromMN = ModuleFindFromName(aModuleName);
    MODULE_INFO *pModuleFromFN = ModuleFindFromFileName(aFileName);
    if (  NULL == pModuleFromMN
       && NULL == pModuleFromFN)
    {
        // Ensure that enough room is available to append a new MODULE_INFO.
        //
        MODULE_INFO *pModule = NULL;
        try
        {
            pModule = new MODULE_INFO;
        }
        catch (...)
        {
            ; // Nothing.
        }

        if (NULL == pModule)
        {
            return NULL;
        }

        // Fill in new MODULE_INFO
        //
        pModule->fpGetClassObject = NULL;
        pModule->fpCanUnloadNow = NULL;
        pModule->fpRegister = NULL;
        pModule->fpUnregister = NULL;
        pModule->hInst = NULL;
        pModule->pModuleName = CopyUTF8(aModuleName);
#ifdef WIN32
        pModule->pFileName = CopyUTF16(aFileName);
#else
        pModule->pFileName = CopyUTF8(aFileName);
#endif // WIN32
        pModule->bLoaded = false;

        if (  NULL != pModule->pModuleName
           && NULL != pModule->pFileName)
        {
            // Add new MODULE_INFO to the end of the list.
            //
            pModule->pNext = NULL;
            if (NULL == g_pModuleLast)
            {
                g_pModuleList = pModule;
            }
            else
            {
                g_pModuleLast->pNext = pModule;
                g_pModuleLast = pModule;
            }
            return pModule;
        }
        else
        {
            // Clean up after failing to copy string.
            //
            if (NULL != pModule->pModuleName)
            {
                delete [] pModule->pModuleName;
                pModule->pModuleName = NULL;
            }

            if (NULL != pModule->pFileName)
            {
                delete [] pModule->pFileName;
                pModule->pFileName = NULL;
            }

            delete [] pModule;
        }
    }
    return NULL;
}

/*! \brief Removes a module from the module table.
 *
 * \param pModule      Module context record to remove and destroy.
 */

static void ModuleRemove(MODULE_INFO *pModule)
{
    MODULE_INFO *p = g_pModuleList;
    MODULE_INFO *q = NULL;

    while (NULL != p)
    {
        if (pModule == p)
        {
            // Unlink from list.
            //
            if (NULL == q)
            {
                g_pModuleList = p->pNext;
            }
            else
            {
                q->pNext = p->pNext;
            }

            // As a precaution, remove any any references in the class id
            // table.  This should have been done when we asked the module to
            // revoke its class ids.
            //
            int i;
            for (i = 0; i < g_nClasses; i++)
            {
                if (g_pClasses[i].pModule == pModule)
                {
                    ClassRemove(g_pClasses[i].ci.cid);
                }
            }

            // Free associated memory.
            //
            if (NULL != p->pModuleName)
            {
                delete [] p->pModuleName;
                p->pModuleName = NULL;
            }

            if (NULL != p->pFileName)
            {
                delete [] p->pFileName;
                p->pFileName = NULL;
            }

            delete p;
            return;
        }

        q = p;
        p = p->pNext;
    }
}

/*! \brief Loads a known module.
 *
 * \param pModule   Module context record.
 */

static void ModuleLoad(MODULE_INFO *pModule)
{
    if (pModule->bLoaded)
    {
        // Module is already in loaded state.
        //
        return;
    }

    pModule->hInst = MOD_OPEN(pModule->pFileName);
    if (NULL != pModule->hInst)
    {
        pModule->fpGetClassObject = (FPGETCLASSOBJECT *)MOD_SYM(pModule->hInst, "mux_GetClassObject");
        pModule->fpCanUnloadNow   = (FPCANUNLOADNOW *)MOD_SYM(pModule->hInst, "mux_CanUnloadNow");
        pModule->fpRegister       = (FPREGISTER *)MOD_SYM(pModule->hInst, "mux_Register");
        pModule->fpUnregister     = (FPUNREGISTER *)MOD_SYM(pModule->hInst, "mux_Unregister");
        if (  NULL != pModule->fpGetClassObject
           && NULL != pModule->fpCanUnloadNow
           && NULL != pModule->fpRegister
           && NULL != pModule->fpUnregister)
        {
            pModule->bLoaded = true;
        }
        else
        {
            MOD_CLOSE(pModule->hInst);
        }
    }
}

/*! \brief Unloads a known module.
 *
 * \param pModule   Module context record.
 */

static void ModuleUnload(MODULE_INFO *pModule)
{
    if (pModule->bLoaded)
    {
        MOD_CLOSE(pModule->hInst);
        pModule->hInst = NULL;
        pModule->fpGetClassObject = NULL;
        pModule->fpCanUnloadNow = NULL;
        pModule->fpRegister = NULL;
        pModule->fpUnregister = NULL;
        pModule->bLoaded = false;
    }
}

/*! \brief Creates an instance of the given class with the given interface.
 *
 * \param  cid   Class ID
 * \param  iid   Interface ID
 * \return       MUX_RESULT
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_CreateInstance(MUX_CID cid, mux_IUnknown *pUnknownOuter, create_context ctx, MUX_IID iid, void **ppv)
{
    MUX_RESULT mr = MUX_S_OK;

    if (  (UseSameProcess & ctx)
       || (  g_ProcessContext == IsMainProcess
          && (UseMainProcess & ctx))
       || (  g_ProcessContext == IsSlaveProcess
          && (UseSlaveProcess & ctx)))
    {
        MODULE_INFO *pModule = ModuleFindFromCID(cid);
        if (NULL != pModule)
        {
            if (pModule == &g_MainModule)
            {
                if (NULL == pModule->fpGetClassObject)
                {
                    mr = MUX_E_CLASSNOTAVAILABLE;
                }
            }
            else if (!pModule->bLoaded)
            {
                ModuleLoad(pModule);
                if (!pModule->bLoaded)
                {
                    mr =  MUX_E_CLASSNOTAVAILABLE;
                }
            }

            if (MUX_SUCCEEDED(mr))
            {
                mux_IClassFactory *pIClassFactory = NULL;
                MUX_RESULT mr = pModule->fpGetClassObject(cid, mux_IID_IClassFactory, (void **)&pIClassFactory);
                if (  MUX_SUCCEEDED(mr)
                   && NULL != pIClassFactory)
                {
                    mr = pIClassFactory->CreateInstance(pUnknownOuter, iid, ppv);
                    pIClassFactory->Release();
                }
            }
        }
    }
#ifdef STUB_SLAVE
    else
    {
        // Out-of-Proc.
        //
        // 1. Send cid and iid to a priori endpoint on the other side and
        //    block until the other side responds with a return frame.
        //
        QUEUE_INFO qiFrame;

        Pipe_InitializeQueueInfo(&qiFrame);
        Pipe_AppendBytes(&qiFrame, sizeof(cid), (UINT8*)(&cid));
        Pipe_AppendBytes(&qiFrame, sizeof(iid), (UINT8*)(&iid));

        mr = Pipe_SendCallPacketAndWait(0, &qiFrame);

        if (MUX_SUCCEEDED(mr))
        {
            MUX_CID cidProxy = 0;
            size_t nWanted = sizeof(cidProxy);
            if (  Pipe_GetBytes(&qiFrame, &nWanted, (UINT8*)(&cidProxy))
               && sizeof(cidProxy) == nWanted)
            {
                // Open an IMarshal interface on the given proxy and pass it the marshal packet.
                //
                mux_IMarshal *pIMarshal = NULL;
                mr = mux_CreateInstance(cidProxy, NULL, UseSameProcess, mux_IID_IMarshal, (void **)&pIMarshal);
                if (MUX_SUCCEEDED(mr))
                {
                    mr = pIMarshal->UnmarshalInterface(&qiFrame, iid, ppv);
                    pIMarshal->Release();
                }
            }
        }
        Pipe_EmptyQueue(&qiFrame);
    }
#endif // STUB_SLAVE
    return mr;
}

/*! \brief Register class ids and factory implemented by the process binary.
 *
 * Modules must pass NULL for pfGetClassObject, but the main program (netmux
 * or stubslave) must pass a non-NULL pfGetClassObject.  For modules, the
 * class factory is obtained by using the mux_GetClassObject export.
 *
 * \param nci                  Number of components to register.
 * \param aci                  Table of component-related info.
 * \param fpGetClassObject     Pointer to Factory capable of creating
 *                             instances of the given components.
 * \return                     MUX_RESULT
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_RegisterClassObjects(int nci, CLASS_INFO aci[], FPGETCLASSOBJECT *fpGetClassObject)
{
    if (  nci <= 0
       || NULL == aci)
    {
        return MUX_E_INVALIDARG;
    }

    // Modules export a mux_GetClassObject handler, but the main program
    // (netmux or stubslave) must pass its handler in here. Also, it doesn't
    // make sense to load and unload netmux.  But, we want to allow the main
    // program to provide module interfaces, so some special-casing is done to
    // allow that.
    //
    if (  (  NULL != g_pModule
          && NULL != fpGetClassObject)
       || (  NULL == g_pModule
          && NULL == fpGetClassObject))
    {
        return MUX_E_INVALIDARG;
    }

    // Verify that the requested class ids are not already registered.
    //
    MODULE_INFO *pModule = NULL;
    int i;
    for (i = 0; i < nci; i++)
    {
        pModule = ModuleFindFromCID(aci[i].cid);
        if (NULL != pModule)
        {
            return MUX_E_INVALIDARG;
        }
    }

    // Find corresponding MODULE_INFO. Since we're the one that requested the
    // module to register its classes, we know which module is registering.
    //
    pModule = g_pModule;
    if (NULL == pModule)
    {
        // These classes are implemented in the main program (netmux or
        // stubslave).
        //
        pModule = &g_MainModule;
        if (NULL != pModule->fpGetClassObject)
        {
            // The main program is attempting to register another handler.
            //
            return MUX_E_FAIL;
        }
    }

    // Make sure there is enough room in the class table for additional class
    // ids.
    //
    if (g_nClassesAllocated < g_nClasses + nci)
    {
        int nAllocate = GrowByFactor(g_nClasses + nci);

        CLASS_INFO_PRIVATE *pNewClasses = NULL;
        try
        {
            pNewClasses = new CLASS_INFO_PRIVATE[nAllocate];
        }
        catch (...)
        {
            ; // Nothing.
        }

        if (NULL == pNewClasses)
        {
            return MUX_E_OUTOFMEMORY;
        }

        if (NULL != g_pClasses)
        {
            int j;
            for (j = 0; j < g_nClasses; j++)
            {
                pNewClasses[j] = g_pClasses[j];
            }

            delete [] g_pClasses;
            g_pClasses = NULL;
        }

        g_pClasses = pNewClasses;
        g_nClassesAllocated = nAllocate;
    }

    // If these classes are implemented in the main program (netmux or
    // stubslave), save the private GetClassObject method.
    //
    if (&g_MainModule == pModule)
    {
        pModule->fpGetClassObject = fpGetClassObject;
    }

    for (i = 0; i < nci; i++)
    {
        ClassAdd(&(aci[i]), pModule);
    }
    return MUX_S_OK;
}

/*! \brief De-register class ids and possibly the handler implemented by the
 *         process binary.
 *
 * \param nci    Number of components to revoke
 * \param aci    Table of component-related info.
 * \return       MUX_RESULT
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_RevokeClassObjects(int nci, CLASS_INFO aci[])
{
    if (  nci <= 0
       || NULL == aci)
    {
        return MUX_E_INVALIDARG;
    }

    // Verify that all class ids in this request are handled by the same module.
    //
    MODULE_INFO *pModule = NULL;
    int i;
    for (i = 0; i < nci; i++)
    {
        MODULE_INFO *q = ModuleFindFromCID(aci[i].cid);
        if (NULL == q)
        {
            // Attempt to revoke a class ids which were never registered.
            //
            return MUX_E_INVALIDARG;
        }
        else if (NULL == pModule)
        {
            pModule = q;
        }
        else if (q != pModule)
        {
            // Attempt to revoke class ids from more than one module.
            //
            return MUX_E_INVALIDARG;
        }
    }

    // If these classes are implemented by the main program (netmux or
    // stubslave), we need to clear the handler as well.
    //
    if (pModule == &g_MainModule)
    {
        pModule->fpGetClassObject = NULL;
    }

    // Remove the requested class ids.
    //
    for (i = 0; i < nci; i++)
    {
        ClassRemove(aci[i].cid);
    }
    return MUX_S_OK;
}

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_RegisterInterfaces(int nii, INTERFACE_INFO aii[])
{
    if (  nii <= 0
       || NULL == aii)
    {
        return MUX_E_INVALIDARG;
    }

    // Make sure there is enough room in the interface table.
    //
    if (g_nInterfacesAllocated < g_nInterfaces + nii)
    {
        int nAllocate = GrowByFactor(g_nInterfaces + nii);

        INTERFACE_INFO *pNewInterfaces = NULL;
        try
        {
            pNewInterfaces = new INTERFACE_INFO[nAllocate];
        }
        catch (...)
        {
            ; // Nothing.
        }

        if (NULL == pNewInterfaces)
        {
            return MUX_E_OUTOFMEMORY;
        }

        if (NULL != g_pInterfaces)
        {
            int j;
            for (j = 0; j < g_nInterfaces; j++)
            {
                pNewInterfaces[j] = g_pInterfaces[j];
            }

            delete [] g_pInterfaces;
            g_pInterfaces = NULL;
        }

        g_pInterfaces = pNewInterfaces;
        g_nInterfacesAllocated = nAllocate;
    }

    for (int i = 0; i < nii; i++)
    {
        InterfaceAdd(&aii[i]);
    }
    return MUX_S_OK;
}

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_RevokeInterfaces(int nii, INTERFACE_INFO aii[])
{
    if (  nii <= 0
       || NULL == aii)
    {
        return MUX_E_INVALIDARG;
    }

    for (int i = 0; i < nii; i++)
    {
        InterfaceRemove(aii[i].iid);
    }
    return MUX_S_OK;
}

/*! \brief Add module to the set of available modules.
 *
 * Modules do not use this.
 *
 * \param UTF8[]   Module @list Name
 * \param UTF8[]   Module File Name
 * \return         MUX_RESULT
 */

#ifdef WIN32
extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_AddModule(const UTF8 aModuleName[], const UTF16 aFileName[])
#else
extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_AddModule(const UTF8 aModuleName[], const UTF8 aFileName[])
#endif // WIN32
{
    MUX_RESULT mr;
    if (NULL == g_pModule)
    {
        // Create new MODULE_INFO.
        //
        MODULE_INFO *pModule = ModuleAdd(aModuleName, aFileName);
        if (NULL != pModule)
        {
            // Ask module to register its classes.
            //
            ModuleLoad(pModule);
            if (pModule->bLoaded)
            {
                g_pModule = pModule;
                mr = pModule->fpRegister();
                g_pModule = NULL;
            }
            else
            {
                mr = MUX_E_FAIL;
            }
        }
        else
        {
            mr = MUX_E_OUTOFMEMORY;
        }
    }
    else
    {
        mr = MUX_E_NOTREADY;
    }
    return mr;
}

/*! \brief Remove module from the set of available modules.
 *
 * Modules do not use this.
 *
 * \param UTF8     Filename of dynamic module to remove.
 * \return         MUX_RESULT
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_RemoveModule(const UTF8 aModuleName[])
{
    MUX_RESULT mr;
    if (NULL == g_pModule)
    {
        MODULE_INFO *pModule = ModuleFindFromName(aModuleName);
        if (NULL != pModule)
        {
            // It is possible for a module to be registered without it
            // necessarily being loaded, but we can't ask it to revoke its
            // class registrations without loading it.
            //
            if (!pModule->bLoaded)
            {
                ModuleLoad(pModule);
            }

            if (pModule->bLoaded)
            {
                // Ask module to revoke its classes.
                //
                g_pModule = pModule;
                mr = pModule->fpUnregister();
                g_pModule = NULL;

                if (MUX_SUCCEEDED(mr))
                {
                    // Attempt to unload module.
                    //
                    mr = pModule->fpCanUnloadNow();
                    if (  MUX_SUCCEEDED(mr)
                       && MUX_S_FALSE != mr)
                    {
                        ModuleUnload(pModule);
                        ModuleRemove(pModule);
                        mr = MUX_S_OK;
                    }
                }
            }
            else
            {
                mr = MUX_E_FAIL;
            }
        }
        else
        {
            mr = MUX_E_NOTFOUND;
        }
    }
    else
    {
        mr = MUX_E_NOTREADY;
    }
    return mr;
}

/*! \brief Return information about a particular module.
 *
 * Modules do not use this.  Notice that the main program module (netmux or
 * stubslave) is not included.
 *
 * \param UTF8     Filename of dynamic module to remove.
 * \param void **  External module info structure.
 * \return         MUX_S_OK if found, MUX_S_FALSE if at end of list,
 *                 MUX_E_INVALIDARG for invalid arguments.
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_ModuleInfo(int iModule, MUX_MODULE_INFO *pModuleInfo)
{
    if (iModule < 0)
    {
        return MUX_E_INVALIDARG;
    }

    MODULE_INFO *pModule = g_pModuleList;
    while (NULL != pModule)
    {
        if (0 == iModule)
        {
            pModuleInfo->bLoaded = pModule->bLoaded;
            pModuleInfo->pName   = pModule->pModuleName;
            return MUX_S_OK;
        }
        iModule--;
        pModule = pModule->pNext;
    }
    return MUX_S_FALSE;
}

/*! \brief Periodic service tick for modules.
 *
 * Modules do not use this.  Notice that the main program module (netmux or
 * stubslave) is not included.
 *
 * \return         MUX_RESULT
 */

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_ModuleMaintenance(void)
{
    // We can query each loaded module and unload the ones that are unloadable.
    //
    MODULE_INFO *pModule = g_pModuleList;
    while (NULL != pModule)
    {
        if (pModule->bLoaded)
        {
            MUX_RESULT mr = pModule->fpCanUnloadNow();
            if (  MUX_SUCCEEDED(mr)
               && MUX_S_FALSE != mr)
            {
                ModuleUnload(pModule);
            }
        }
        pModule = pModule->pNext;
    }
    return MUX_S_OK;
}

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_InitModuleLibrary(process_context ctx, PipePump *fpPipePump)
{
    if (IsUninitialized == g_ProcessContext)
    {
        g_ProcessContext = ctx;
#if defined(STUB_SLAVE)
        if (NULL != fpPipePump)
        {
            // Save pipepump callback. We need to design in a FIFO write
            // callback to netmux.  netmux should provide incoming and
            // outgoing streams.  The pipepump, write, and read packet
            // handlers need to talk to each other in terms of call-level.
            // pipepump will block until a certain call-level is handled by a
            // return. The read packet handler should return the current call
            // level so that pipepump can determine whether that level has
            // been achieved.
            //
            // The module library should deal with packets, call levels, and
            // disconnection clean. The main program (stub or netmux) can
            // handle file descriptors, process spawning, and errors.
            //
        }
#endif
        return MUX_S_OK;
    }
    else
    {
        return MUX_E_FAIL;
    }
}

extern "C" MUX_RESULT DCL_EXPORT DCL_API mux_FinalizeModuleLibrary(void)
{
    g_ProcessContext = IsUninitialized;
    return MUX_S_OK;
}

/*! \brief Receive and parse data stream from stubslave
 *
 * Called from both the main shovechars() loop as well as the pipepump() loop,
 * this function parses data from the stubslave.  Some potential actions
 * include unblocking the return of a RPC to the other side as well as calls
 * from the other side which may ultimate cause other RPC calls to the
 * stubslave.
 *
 * \return         bool    An indication of whether to continue probably.
 */

extern "C" bool DCL_EXPORT DCL_API mux_ReceiveData(size_t nBuffer, const void *pBuffer)
{
    return false;
}

void Pipe_InitializeQueueInfo(QUEUE_INFO *pqi)
{
    pqi->pHead = NULL;
    pqi->pTail = NULL;
}

void Pipe_AppendBytes(QUEUE_INFO *pqi, size_t n, const UINT8 *p)
{
    if (  0 != n
       || NULL != p)
    {
        // Continue copying data to the end of the queue until it is all consumed.
        //
        QUEUE_BLOCK *pBlock = NULL;
        while (0 < n)
        {
            // We need an empty or partially filled QUEUE_BLOCK.
            //
            if (  NULL == pqi->pTail
               || pqi->pTail->aBuffer + QUEUE_BLOCK_SIZE <= pqi->pTail->pBuffer + pqi->pTail->nBuffer)
            {
                // The last block is full or not there, so allocate a new QUEUE_BLOCK.
                //
                try
                {
                    pBlock = new QUEUE_BLOCK;
                }
                catch (...)
                {
                    ; // Nothing.
                }

                if (NULL != pBlock)
                {
                    pBlock->pNext   = NULL;
                    pBlock->pPrev   = NULL;
                    pBlock->pBuffer = pBlock->aBuffer;
                    pBlock->nBuffer = 0;
                }
                else
                {
                    // TODO: Out of memory.
                    //
                    return;
                }

                // Append the newly allocated block to the end of the queue.
                //
                if (NULL == pqi->pTail)
                {
                    pqi->pHead = pBlock;
                    pqi->pTail = pBlock;
                }
                else
                {
                    pBlock->pPrev = pqi->pTail;
                    pqi->pTail->pNext = pBlock;
                    pqi->pTail = pBlock;
                }
            }
            else
            {
                pBlock = pqi->pTail;
            }

            // Allocate space out of last QUEUE_BLOCK
            //
            char  *pFree = pBlock->pBuffer + pBlock->nBuffer;
            size_t nFree = QUEUE_BLOCK_SIZE - pBlock->nBuffer - (pBlock->pBuffer - pBlock->aBuffer);
            size_t nCopy = nFree;
            if (n < nCopy)
            {
                nCopy = n;
            }

            memcpy(pFree, p, nCopy);
            n -= nCopy;
            pBlock->nBuffer += nCopy;
        }
    }
}

void Pipe_EmptyQueue(QUEUE_INFO *pqi)
{
    if (NULL != pqi)
    {
        QUEUE_BLOCK *pBlock = pqi->pHead;

        // Free all the QUEUE_BLOCKs finally the owning QUEUE_INFO structure.
        //
        while (NULL != pBlock)
        {
            QUEUE_BLOCK *qBlock = pBlock->pNext;
            delete pBlock;
            pBlock = qBlock;
        }

        pqi->pHead = NULL;
        pqi->pTail = NULL;
    }
}

bool Pipe_GetByte(QUEUE_INFO *pqi, UINT8 ach[0])
{
    QUEUE_BLOCK *pBlock;

    if (  NULL != pqi
       && NULL != (pBlock = pqi->pHead))
    {
        // Advance over empty blocks.
        //
        while (  0 == pBlock->nBuffer
              && NULL != pBlock->pNext)
        {
            pqi->pHead = pBlock->pNext;
            if (NULL == pqi->pHead)
            {
                pqi->pTail = NULL;
            }
            delete pBlock;
            pBlock = pqi->pHead;
        }

        // If there is a block left on the list, it will have something.
        //
        if (NULL != pBlock)
        {
            ach[0] = pBlock->pBuffer[0];
            pBlock->pBuffer++;
            pBlock->nBuffer--;

            return true;
        }
    }
    return false;
}

bool Pipe_GetBytes(QUEUE_INFO *pqi, size_t *pn, UINT8 *pch)
{
    size_t nCopied = 0;
    QUEUE_BLOCK *pBlock;

    if (  NULL != pqi
       && NULL != pn)
    {
        size_t nWantedBytes = *pn;
        pBlock = pqi->pHead;
        while (  NULL != pBlock
              && 0 < nWantedBytes)
        {
            // Advance over empty blocks.
            //
            while (  0 == pBlock->nBuffer
                  && NULL != pBlock->pNext)
            {
                pqi->pHead = pBlock->pNext;
                if (NULL == pqi->pHead)
                {
                    pqi->pTail = NULL;
                }
                delete pBlock;
                pBlock = pqi->pHead;
            }

            // If there is a block left on the list, it will have something.
            //
            if (NULL != pBlock)
            {
                size_t nCopy = pBlock->nBuffer;
                if (nWantedBytes < nCopy)
                {
                    nCopy = nWantedBytes;
                }
                memcpy(pch, pBlock->pBuffer, nCopy);

                pBlock->pBuffer += nCopy;
                pBlock->nBuffer -= nCopy;
                nWantedBytes -= nCopy;
                pch += nCopy;
                nCopied += nCopy;
            }
        }

        *pn = nCopied;
        return true;
    }
    return false;
}

MUX_RESULT Pipe_SendCallPacketAndWait(int nChannel, QUEUE_INFO *pqi)
{
    return -(__LINE__);
}
