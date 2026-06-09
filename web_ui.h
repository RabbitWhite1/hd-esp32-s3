#pragma once
#include <Arduino.h>

// LAN-only to-do list backend. Serves a page where anyone on the same Wi-Fi can
// edit a checklist (tick items, add a row with "+", save); the frontend (.ino)
// renders the list in a box on the LCD. State lives in RAM only — it clears on reboot.
void webBegin();   // register routes and start the HTTP server (call after Wi-Fi is up)
void webHandle();  // service pending HTTP clients (call often from loop())

int webTodoCount();              // number of to-do items currently stored
const char *webTodoText(int i);  // text of item i ("" if out of range)
bool webTodoDone(int i);         // whether item i is ticked (false if out of range)
