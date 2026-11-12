#ifndef FAST5_THREAD_WIN32_H
#define FAST5_THREAD_WIN32_H

#include <stdint.h>

/*
 * Minimal pthread surface over Win32 for the translated game.
 *
 * pthread_t is the 32-bit guest handle the shipped code stores; this layer maps
 * it to a Win32 thread and a guest Cpu/stack. Mutex handles are likewise stored
 * in the guest's 4-byte pthread_mutex_t object.
 */

int thread_win32_create(uint8_t *mem, uint32_t mem_size, uint32_t thread_ptr,
                        uint32_t start, uint32_t arg);
int thread_win32_join(uint8_t *mem, uint32_t thread_id, uint32_t retval_ptr);
uint32_t thread_win32_self(void);

int thread_win32_mutex_init(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_lock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_trylock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_unlock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_destroy(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);

#endif
