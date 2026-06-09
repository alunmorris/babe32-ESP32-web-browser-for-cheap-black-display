/*
 * 50-URL navigation history stack in PSRAM
 * Written by Alun Morris and Claude Code
 *
 * 060326 50-URL navigation history stack in PSRAM
 */
#pragma once
#include <stdbool.h>

#define HISTORY_MAX     50
#define HISTORY_URL_LEN 512

void        history_init();
void        history_push(const char *url);
const char *history_back();
const char *history_forward();
const char *history_current();
bool        history_can_back();
bool        history_can_forward();
