/* Bluetooth LE transport for remote pages: Nordic UART Service (NUS). See remote.h. */
#ifndef BLE_H_
#define BLE_H_

/* Enable Bluetooth and start advertising. Returns 0 or a negative errno. */
int ble_start(void);

/* Queue one reply line for the subscribed peers (never blocks, drops if the queue is full). */
void ble_send_line(const char *line);

#endif /* BLE_H_ */
