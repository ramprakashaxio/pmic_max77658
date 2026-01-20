#ifndef BLE_APP_H
#define BLE_APP_H

/* Start the BLE application thread(s) */
void start_ble_thread(void);

/* Prepare BLE for shutdown (stop advertising, disable notifications) */
void ble_prepare_for_shutdown(void);

#endif /* BLE_APP_H */
