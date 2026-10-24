#include "app.h"

// First-boot persistence verifies complete snapshots; leave room for nested NVS calls.
SET_LOOP_TASK_STACK_SIZE(16384);

void setup() { firmware::begin(); }
void loop() { firmware::tick(); }
