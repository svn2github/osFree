/* os2app client-side API */

#ifndef __OS3_APP_H__
#define __OS3_APP_H__

#ifdef __cplusplus
  extern "C" {
#endif

APIRET AppClientGetLoadError(l4_os3_cap_idx_t client_id,
                             PBYTE  szLoadError,
                             PULONG pcbLoadError);

APIRET AppClientTerminate(l4_os3_cap_idx_t client_id);

APIRET AppClientAreaAdd(l4_os3_cap_idx_t client_id,
                        PVOID pAddr,
                        ULONG ulSize,
                        ULONG ulFlags);

APIRET AppClientDataspaceAttach(l4_os3_cap_idx_t   client_id,
                                PVOID              pAddr,
                                l4_os3_dataspace_t ds,
                                ULONG              ulRights);

APIRET AppClientDataspaceRelease(l4_os3_cap_idx_t   client_id,
                                 l4_os3_dataspace_t ds);

#ifdef __cplusplus
  }
#endif

#endif
