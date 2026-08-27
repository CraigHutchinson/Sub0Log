#ifndef SUB0LOG_ABI_H
#define SUB0LOG_ABI_H

/* sub0log_abi.h -- the plugin-facing boundary (REQUIREMENTS.md R4).
 *
 * C, dependency-free, allocation-free: nothing crossing this boundary owns
 * memory or runs C++ (R4.2). A plugin compiles this header only and never
 * links the library (R4.1); the host exports one symbol returning the table.
 *
 * The table is size-versioned: a plugin checks `size` before touching a
 * field, so the table can grow without breaking old plugins. Function order
 * is append-only forever.
 *
 * The host-side implementation is sub0log/abi_host.hpp; sub0log/detail
 * (headers that stay C++) never appears here. The dlopen round trip is
 * tests/system/abi.test.cpp.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUB0LOG_ABI_VERSION 1u

/* One record emission, prepared by the plugin, written by the host's
 * producer path. Severity values match sub0log::Severity. */
typedef struct Sub0LogAbiRecord {
    uint64_t site_id;       /* plugin-side descriptor address */
    uint32_t subsystem_id;
    uint8_t severity;
    uint8_t reserved[3];
    const void* payload;    /* pre-encoded argument bytes, or NULL */
    uint32_t payload_bytes;
} Sub0LogAbiRecord;

typedef struct Sub0LogAbiV1 {
    /* sizeof(Sub0LogAbiV1) as compiled into the host; the capability check. */
    uint32_t size;
    uint32_t version; /* SUB0LOG_ABI_VERSION */

    /* Writes the site-definition record for a plugin call site. The plugin
     * calls this once per site before (or lazily at) first emit; the host
     * writes it into the stream so decoding outlives the plugin (R4.3). */
    void (*define_site)(uint64_t site_id, uint32_t subsystem_id, uint8_t severity,
                        const char* format_text, const char* file, uint32_t line,
                        const uint8_t* arg_type_codes, uint8_t arg_count);

    /* Emits one message record with the current time and the correlation id
     * in scope on the calling thread. Never blocks; a drop is counted. */
    void (*emit)(const Sub0LogAbiRecord* record);

    /* The correlation id in scope on the calling thread (R6.1 across the
     * plugin boundary), 0 when none. */
    uint64_t (*current_correlation)(void);
} Sub0LogAbiV1;

/* The host application exports this; a plugin resolves it by name. Returns
 * NULL when no logger is bound in the host. */
typedef const Sub0LogAbiV1* (*Sub0LogAbiGetter)(void);
#define SUB0LOG_ABI_GETTER_NAME "sub0log_abi_v1"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SUB0LOG_ABI_H */
