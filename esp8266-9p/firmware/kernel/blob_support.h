/* blob_support - SDK support layer for running the RTOS Wi-Fi
 * blobs (libpp/libnet80211/libphy) on the Plan 9 substrate with
 * no FreeRTOS. Implements the non-OSI part of the B16 closure:
 * critical sections, heap_caps over the NONOS heap, log/event/
 * timer stubs, and a RAM NVS shim. ROM-provided symbols
 * (ets_*, lldesc_build_chain, Cache_*) are NOT here; the NONOS
 * linker script provides them in a blobtest image.
 */
#ifndef BLOB_SUPPORT_H
#define BLOB_SUPPORT_H

/* Nothing to declare: this file exists so blobtest apps can
 * force-link the support objects. Symbols are the interface. */

#endif
