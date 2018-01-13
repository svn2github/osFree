#ifndef __L4ENV_PLATFORM_SEMAPHORE_H__
#define __L4ENV_PLATFORM_SEMAPHORE_H__

#ifdef __cplusplus
  extern "C" {
#endif

#include <l4/semaphore/semaphore.h>
#include <l4/lock/lock.h>

typedef l4semaphore_t l4_os3_semaphore_t;
typedef l4lock_t l4_os3_lock_t;

#ifdef __cplusplus
  }
#endif

#endif /* __L4ENV_PLATFORM_SEMAPHORE_H__ */
