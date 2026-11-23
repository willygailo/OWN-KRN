#ifndef OWNKRN_DRIVERS_SPEAKER_H
#define OWNKRN_DRIVERS_SPEAKER_H

#include <kernel/types.h>

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

#endif /* OWNKRN_DRIVERS_SPEAKER_H */
