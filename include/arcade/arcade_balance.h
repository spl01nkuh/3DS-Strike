#ifndef ARCADE_BALANCE_H
#define ARCADE_BALANCE_H

/* Compatibility shim for upstream (crowded-street/3sx) sources.
 *
 * Upstream gates a growing number of CPS3-accuracy behaviours behind
 * ArcadeBalance_IsEnabled(). There it is an opt-in feature that requires an
 * arcade ROM and a config toggle, so it is false in a default build. This port
 * has no arcade-balance option at all, which means the port's correct
 * behaviour is exactly upstream's default.
 *
 * Providing the predicate as a constant-false inline lets upstream files be
 * carried over verbatim -- including their `#include "arcade/arcade_balance.h"`
 * -- instead of hand-rewriting every gate during a sync, which is where
 * transcription mistakes come from. The compiler folds the guarded branches
 * away, so this costs nothing at runtime.
 *
 * If arcade balance is ever actually ported, replace this with the real
 * implementation and the call sites need no changes.
 */

#include <stdbool.h>

static inline bool ArcadeBalance_IsEnabled(void) {
    return false;
}

#endif
