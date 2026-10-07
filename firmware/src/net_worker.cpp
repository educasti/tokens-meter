// Single shared network worker. See net_worker.h for why one stack replaces the
// three per-module stacks, and for the internal-RAM placement requirement.
//
// Hardware-only: the native sim excludes this file (platformio.ini) exactly like
// ota_pull.cpp / usage_pull.cpp; the sim has no FreeRTOS tasks and no radio.
#include "net_worker.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>

// One 12 KB internal-RAM stack, core 0 -- identical to the stack each of the
// three former per-module workers used, so the TLS-heavy body fits unchanged.
#define NET_TASK_STACK 12288
#define NET_TASK_PRIO  1
#define NET_TASK_CORE  0

struct net_job_t {
    net_job_fn fn;
    void*      arg;
};

// Depth-1 mailbox: at most one job is queued, and the single worker runs it.
static QueueHandle_t s_queue = nullptr;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int  s_inflight = 0;   // jobs queued or running (guarded by s_mux)

static void net_worker_task(void* arg) {
    (void)arg;
    net_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            if (job.fn) job.fn(job.arg);
            portENTER_CRITICAL(&s_mux);
            if (s_inflight > 0) s_inflight--;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}

void net_worker_init(void) {
    if (s_queue) return;   // idempotent: created once at boot

    s_queue = xQueueCreate(1, sizeof(net_job_t));
    if (!s_queue) {
        Serial.println("NET: shared worker queue alloc failed");
        return;
    }

    // Default (internal-RAM) stack allocation -- mandatory, because the jobs do
    // NVS/flash writes and a PSRAM stack asserts in
    // spi_flash_disable_interrupts_caches.
    if (xTaskCreatePinnedToCore(net_worker_task, "net_worker", NET_TASK_STACK,
                                nullptr, NET_TASK_PRIO, nullptr, NET_TASK_CORE) != pdPASS) {
        Serial.println("NET: shared worker task alloc failed");
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return;
    }

    Serial.printf("NET: shared worker up (stack=%u internal, core=%d)\n",
                  (unsigned)NET_TASK_STACK, (int)NET_TASK_CORE);
}

bool net_worker_submit(net_job_fn fn, void* arg) {
    if (!s_queue || !fn) return false;

    // Count the job before sending so net_worker_busy() is true from the moment
    // it can be picked up; undo the count if the slot is already occupied.
    portENTER_CRITICAL(&s_mux);
    s_inflight++;
    portEXIT_CRITICAL(&s_mux);

    net_job_t job = { fn, arg };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        if (s_inflight > 0) s_inflight--;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    return true;
}

bool net_worker_busy(void) {
    portENTER_CRITICAL(&s_mux);
    bool busy = s_inflight > 0;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}
