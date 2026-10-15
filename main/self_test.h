// main/self_test.h
#ifndef SELF_TEST_H
#define SELF_TEST_H

// Starts a background task that exercises the HTTP endpoints over the
// device's own SoftAP interface and reports PASS/FAIL on the USB serial
// console. Used because the build host has no WiFi interface to run the
// external curl tests.
void start_self_test(void);

#endif // SELF_TEST_H