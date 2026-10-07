#pragma once
#include <stdbool.h>

// Single shared network worker for every blocking WiFi/TLS/flash job.
//
// The pull-OTA engine (ota_pull.cpp), the backend usage pull (usage_pull.cpp)
// and the pairing poll (usage_pair.cpp) all block on a WiFi join, an SNTP sync
// and a TLS handshake, and all three do NVS/flash writes. Before this module
// each one created its own dedicated 12 KB internal-RAM FreeRTOS task, so three
// stacks (36 KB) were resident at boot even though the three can never run at
// the same time (they are mutually exclusive on the WiFi owner OTA_WIFI_PULL).
// On hardware that starved the internal heap: the largest contiguous block fell
// from ~90 KB to ~32 KB and the manifest TLS handshake failed with
// MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512), with pairing following the same way.
//
// This module creates ONE 12 KB internal-RAM task. The *_tick() schedulers stay
// on the Arduino loop task and hand the due job (a function pointer) to the
// worker; whoever is due runs on the shared stack. Freeing the other two stacks
// restores ~24 KB of contiguous internal RAM, which is what the handshake needs.
//
// Stack placement: the stack MUST stay in internal RAM -- the jobs perform
// NVS/flash writes and a PSRAM-backed stack asserts in
// spi_flash_disable_interrupts_caches. xTaskCreatePinnedToCore's default
// allocation is internal, which is what we want; MALLOC_CAP_SPIRAM is never
// requested.
//
// Threading: net_worker_submit() is called from the Arduino loop task (inside
// each per-module *_tick()) and never from the NimBLE host task. A single slot
// holds one job at a time, so a submit fails while another module's job is
// queued or running -- exactly the mutual exclusion the WiFi owner enforces.
//
// Hardware-only: the implementation lives in net_worker.cpp, which the native
// sim excludes (platformio.ini), like ota_pull.cpp / usage_pull.cpp. The
// transport-free header is safe for shared code to include on every board.

// A job body. Runs on the shared worker; the job arbitrates the radio itself
// with ota_wifi_acquire(OTA_WIFI_PULL) / ota_wifi_release(), as before.
typedef void (*net_job_fn)(void* arg);

// Create the one shared worker task (idempotent). Call once at boot from
// main.cpp, before any *_init() that may submit jobs.
void net_worker_init(void);

// Queue one job on the shared worker. Never blocks. Returns false when the
// single slot is already occupied; the caller must revert its own "active"
// state and retry on a later tick.
bool net_worker_submit(net_job_fn fn, void* arg);

// True while a job is queued or running. Lets a scheduler avoid consuming a
// one-shot request (e.g. a BLE OTA command) it could not submit yet.
bool net_worker_busy(void);
