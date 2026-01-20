#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#include "data_manager.h"

/* Initialize the NVS file system */
int storage_init(void);

/* Save a batch to flash (Circular Buffer) */
int storage_save_batch(const patient_batch_t *batch);

/* Read the oldest batch WITHOUT deleting it (Safe Peek) */
int storage_peek_next_batch(patient_batch_t *batch);

/* Delete the oldest batch (Advance Read Pointer) */
int storage_drop_next_batch(void);

/* Check if there is pending data */
bool storage_has_data(void);

/* Get number of batches currently stored */
uint16_t storage_pending_count(void);

#endif /* STORAGE_MANAGER_H */
