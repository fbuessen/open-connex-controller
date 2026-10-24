#pragma once
#include "access.h"
#include <Arduino.h>
namespace firmware {
extern connex::Access access;
extern SemaphoreHandle_t accessMutex;
// All callers hold accessMutex except initialization before HTTP starts.
bool beginSecurity();
void pollSecurity();
bool hashBusy();
}
