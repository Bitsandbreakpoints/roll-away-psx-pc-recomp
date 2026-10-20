/*
 * log.h - the PC port's log file (roll_away.log next to the exe).
 */
#ifndef PSX_LOG_H
#define PSX_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a line to the log. */
void psx_log(const char *format, ...);

/* Logs the first call of a library function that is not written yet. */
void psx_stub(const char *function);

#ifdef __cplusplus
}
#endif

#define PSX_STUB()      psx_stub(__func__)

#endif /* PSX_LOG_H */
