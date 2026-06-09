/*
 * On-screen debug log
 * Written by Alun Morris and Claude Code
 *
 * 120326 On-screen debug log
 */
#pragma once

// Initialize the debug log (call before any dbg() calls)
void dbglog_init();

// Add a message to the log (thread-safe, any core)
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Get the full log text (for display). Returns pointer to static buffer.
const char *dbglog_text();
