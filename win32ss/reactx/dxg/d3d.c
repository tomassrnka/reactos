/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Native driver for dxg implementation
 * FILE:             win32ss/reactx/dxg/d3d.c
 * PROGRAMER:        Sebastian Gasiorek (sebastian.gasiorek@reactos.org)
 */

#include <string.h>
#include <dxg_int.h>
#include <pseh/pseh2.h>

DWORD
NTAPI
DxDdCanCreateD3DBuffer(
    HANDLE DdHandle,
    PDD_CANCREATESURFACEDATA SurfaceData)
{
    PEDD_DIRECTDRAW_LOCAL peDdL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl;
    DWORD RetVal = DDHAL_DRIVER_NOTHANDLED;

    peDdL = (PEDD_DIRECTDRAW_LOCAL)DdHmgLock(DdHandle, ObjType_DDLOCAL_TYPE, FALSE);
    if (!peDdL)
        return RetVal;

    peDdGl = peDdL->peDirectDrawGlobal2;
    gpEngFuncs.DxEngLockHdev(peDdGl->hDev);

    // assign out DirectDrawGlobal to SurfaceData
    SurfaceData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;

    if (peDdGl->d3dBufCallbacks.CanCreateD3DBuffer)
        RetVal = peDdGl->d3dBufCallbacks.CanCreateD3DBuffer(SurfaceData);

    gpEngFuncs.DxEngUnlockHdev(peDdGl->hDev);
    InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);

    return RetVal;
}

static
VOID
intDdSetCreateSurfaceResult(DD_CREATESURFACEDATA *pDdCreateSurfaceData, HRESULT hr)
{
    _SEH2_TRY
    {
        pDdCreateSurfaceData->ddRVal = hr;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        NOTHING;
    }
    _SEH2_END;
}

DWORD
FASTCALL
intDdCreateSurfaceOrBuffer(HANDLE hDirectDrawLocal, 
                           PEDD_SURFACE pDdSurfList, DDSURFACEDESC2 *a3, 
                           DD_SURFACE_GLOBAL *pDdSurfGlob, 
                           DD_SURFACE_LOCAL *pDdSurfLoc, 
                           DD_SURFACE_MORE *pDdSurfMore, 
                           DD_CREATESURFACEDATA *pDdCreateSurfaceData, 
                           PVOID Address)
{
  PEDD_DIRECTDRAW_LOCAL peDdL = NULL;
  PEDD_DIRECTDRAW_GLOBAL peDdGl = NULL;
  DD_SURFACE_LOCAL *pCurSurfLocal;
  DD_SURFACE_GLOBAL *pCurSurfGlobal;
  DD_SURFACE_MORE *pCurSurfMore;
  PEDD_SURFACE pCurSurf;

  ULONG CurSurf;
  ULONG SurfaceCount;
  DWORD SurfaceCaps;

  if (!pDdCreateSurfaceData)
      return FALSE;

  /* The create data and the surface local data are caller memory */
  _SEH2_TRY
  {
      ProbeForWrite(pDdCreateSurfaceData, sizeof(*pDdCreateSurfaceData), 1);
      SurfaceCount = pDdCreateSurfaceData->dwSCnt;
  }
  _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
  {
      _SEH2_YIELD(return FALSE);
  }
  _SEH2_END;

  /* The arrays below hold SurfaceCount entries of the largest of these types */
  if (!SurfaceCount || SurfaceCount > MAXULONG / sizeof(EDD_SURFACE))
  {
      intDdSetCreateSurfaceResult(pDdCreateSurfaceData, E_FAIL);
      return FALSE;
  }

  _SEH2_TRY
  {
      ProbeForRead(pDdSurfLoc, sizeof(*pDdSurfLoc), 1);
      SurfaceCaps = pDdSurfLoc->ddsCaps.dwCaps;
  }
  _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
  {
      _SEH2_YIELD(return FALSE);
  }
  _SEH2_END;

  peDdL = (PEDD_DIRECTDRAW_LOCAL)DdHmgLock(hDirectDrawLocal, ObjType_DDLOCAL_TYPE, FALSE);
  if (!peDdL)
      return FALSE;

  peDdGl = peDdL->peDirectDrawGlobal2;

  if (!(SurfaceCaps & DDSCAPS_VISIBLE) && !(peDdGl->ddCallbacks.dwFlags & DDHAL_CB32_CREATESURFACE))
  {
      InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);
      intDdSetCreateSurfaceResult(pDdCreateSurfaceData, E_FAIL);
      return FALSE;
  }

  pDdSurfList = (PEDD_SURFACE)EngAllocMem(FL_ZERO_MEMORY, SurfaceCount * sizeof(EDD_SURFACE), TAG_GDDP);
  pDdSurfGlob = (DD_SURFACE_GLOBAL *)EngAllocMem(FL_ZERO_MEMORY, SurfaceCount * sizeof(DD_SURFACE_GLOBAL), TAG_GDDP);
  pDdSurfLoc = (DD_SURFACE_LOCAL *)EngAllocMem(FL_ZERO_MEMORY, SurfaceCount * sizeof(DD_SURFACE_LOCAL), TAG_GDDP);
  pDdSurfMore = (DD_SURFACE_MORE *)EngAllocMem(FL_ZERO_MEMORY, SurfaceCount * sizeof(DD_SURFACE_MORE), TAG_GDDP);
  if (!pDdSurfList || !pDdSurfGlob || !pDdSurfLoc || !pDdSurfMore)
  {
      if (pDdSurfList) EngFreeMem(pDdSurfList);
      if (pDdSurfGlob) EngFreeMem(pDdSurfGlob);
      if (pDdSurfLoc) EngFreeMem(pDdSurfLoc);
      if (pDdSurfMore) EngFreeMem(pDdSurfMore);
      InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);
      intDdSetCreateSurfaceResult(pDdCreateSurfaceData, E_OUTOFMEMORY);
      return FALSE;
  }

  gpEngFuncs.DxEngLockShareSem();
  gpEngFuncs.DxEngLockHdev(peDdGl->hDev);

  // create all surface objects
  for (CurSurf = 0; CurSurf < SurfaceCount; CurSurf++)
  {
      pCurSurf       = &pDdSurfList[CurSurf];
      pCurSurfLocal  = &pDdSurfLoc[CurSurf];
      pCurSurfGlobal = &pDdSurfGlob[CurSurf];
      pCurSurfMore   = &pDdSurfMore[CurSurf];

      pCurSurf = intDdCreateNewSurfaceObject(
                          peDdL,
                          pCurSurf,
                          pCurSurfGlobal,
                          pCurSurfLocal,
                          pCurSurfMore);
      Address = pCurSurf;
  }

  gpEngFuncs.DxEngUnlockHdev(peDdGl->hDev);
  gpEngFuncs.DxEngUnlockShareSem();

  return DDHAL_DRIVER_HANDLED;
}

DWORD
NTAPI
DxDdCreateD3DBuffer(
    HANDLE hDirectDrawLocal,
    PEDD_SURFACE pDdSurfList,
    DDSURFACEDESC2 *a3,
    DD_SURFACE_GLOBAL *pDdSurfGlob,
    DD_SURFACE_LOCAL *pDdSurfLoc,
    DD_SURFACE_MORE *pDdSurfMore,
    DD_CREATESURFACEDATA *pDdCreateSurfaceData,
    PVOID Address)
{
    return intDdCreateSurfaceOrBuffer(hDirectDrawLocal, pDdSurfList, a3, pDdSurfGlob, pDdSurfLoc, pDdSurfMore, pDdCreateSurfaceData, Address);
}
