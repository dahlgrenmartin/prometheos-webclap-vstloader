/*
 * prometheos.runtime/1: a WebCLAP extension for plugins whose real work runs
 * in a separate runtime page (here: Wine inside Boxedwine) instead of the
 * plugin's own module.
 *
 * The plugin names its runtime page; the host loads it once per URL in a
 * hidden frame and, when the module's memory is shared, hands that frame the
 * module's WebAssembly.Memory, so the runtime and the plugin's process() can
 * exchange audio through shared memory without the main thread. Control
 * messages travel as opaque byte frames in both directions, like
 * clap.webview/3's send/receive.
 *
 * Hosts only load runtimes they trust (an allowlist or a pinned integrity
 * hash): a runtime page runs with the host's origin.
 */
#ifndef PROMETHEOS_RUNTIME_H
#define PROMETHEOS_RUNTIME_H

#include <clap/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char PROMETHEOS_EXT_RUNTIME[] = "prometheos.runtime/1";

typedef struct prometheos_plugin_runtime {
    /* Writes the runtime page's absolute URL (NUL-terminated, at most
     * `capacity` bytes) and returns its length without the NUL, or a
     * negative value when the plugin needs no runtime. [main-thread] */
    int32_t(CLAP_ABI *get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);

    /* A frame from the runtime. Returns false if it was not understood.
     * [main-thread] */
    bool(CLAP_ABI *receive)(const clap_plugin_t *plugin, const void *buffer, uint32_t size);
} prometheos_plugin_runtime_t;

typedef struct prometheos_host_runtime {
    /* Queues a frame for the plugin's runtime; the host delivers frames in
     * order once the runtime is up. Returns false if the host cannot
     * deliver it (no runtime). [main-thread] */
    bool(CLAP_ABI *send)(const clap_host_t *host, const void *buffer, uint32_t size);
} prometheos_host_runtime_t;

#ifdef __cplusplus
}
#endif

#endif
