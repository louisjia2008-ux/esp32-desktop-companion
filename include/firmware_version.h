#pragma once
#if defined(SENSOR_SERVICE_ENABLED) && (defined(SENSOR_LD2410C_RX_PROBE) || defined(SENSOR_SCD41_PROBE))
#error "Production sensor service and diagnostic probes cannot own the same buses"
#endif
#if defined(SENSOR_DUAL_PROBE) && (!defined(SENSOR_LD2410C_RX_PROBE) || !defined(SENSOR_SCD41_PROBE))
#error "Dual-sensor diagnostics require both sensor probes"
#endif
#if defined(SENSOR_LD2410C_RX_PROBE) && defined(SENSOR_SCD41_PROBE) && !defined(SENSOR_DUAL_PROBE)
#error "Single-sensor probes must not be enabled together"
#endif
#if defined(SENSOR_SERVICE_ENABLED)
#define DEVCOMPANION_VERSION "0.9.2"
#elif defined(SENSOR_DUAL_PROBE)
#define DEVCOMPANION_VERSION "0.8.1-sensors-t1"
#elif defined(SENSOR_LD2410C_RX_PROBE)
#define DEVCOMPANION_VERSION "0.8.1-radar-rx2"
#elif defined(SENSOR_SCD41_PROBE)
#define DEVCOMPANION_VERSION "0.8.1-scd41-t1"
#else
#define DEVCOMPANION_VERSION "0.8.1"
#endif
