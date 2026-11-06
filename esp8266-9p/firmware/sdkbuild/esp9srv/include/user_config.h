#ifndef __USER_CONFIG_H__
#define __USER_CONFIG_H__

/* Phase A firmware config.
 *
 * Optional build-time station provisioning: define these to make
 * the firmware join an AP at boot (SoftAP stays up). Intended for
 * bench tests where no client can join the SoftAP first to write
 * /wifi/ctl. Leave undefined for the normal SoftAP bootstrap.
 */
/* #define ESP9_PROVISION_SSID "my-ap" */
/* #define ESP9_PROVISION_KEY  "my-key" */

#endif
